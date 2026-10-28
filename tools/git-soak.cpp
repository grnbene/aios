// The kernel's git, held against real git on real repository histories, for hours.
//
//   aios-git-soak --corpus DIR --corpus-def FILE --work DIR --findings DIR --ledger DIR [--hours H]
//   aios-git-soak ... --replay FINDINGS/<id>       re-run exactly the operands of one finding
//   aios-git-soak ... --gate --min-hours H          the release gate (no soak is run)
//   aios-git-soak ... --inject-fault N              prove the gate bites: corrupt every Nth AIOS answer
//
// tools/git-merge-check.cpp is a CI pass over generated inputs with one fixed seed. This is
// the other thing: an unattended soak whose inputs are real merge commits, real blob pairs and
// real packfiles from pinned mirrors of real repositories, each iteration under a fresh seed
// that is logged, so every disagreement can be replayed from its operands.
//
// Real git is the only opinion that counts. Every check puts the same question to the
// kernel's own implementation and to `git`, and the two answers must be identical bytes:
//   treemerge   git_merge_commits               vs  git merge-tree --write-tree  (+ fsck --strict
//                                                    on every object AIOS wrote)
//   merge3      git_merge3 (+ -X ours/theirs)   vs  git merge-file -p            (on blobs a real merge changed twice)
//   diff        git_unified_hunks               vs  git diff <blob> <blob>       (on blobs a real commit changed)
//   mergebase   git_merge_bases/git_is_ancestor vs  git merge-base --all / --is-ancestor
//   objects     read_object / flatten_tree      vs  git cat-file / git ls-tree -r  (default, deep-delta
//                                                    and REF_DELTA repacks of the same repository)
//   renames     git_detect_renames              vs  git diff-tree -M
//   packwrite   git_build_pack                  vs  git index-pack --strict + verify-pack
//   synth_*     the old generators, under fresh seeds
//
// A disagreement is never "noted": it writes a reproducer under findings/ and the run exits 1.
// The release gate refuses while any finding is open. A missing git, a different git, an
// empty corpus or a harness that cannot move bytes faithfully is exit 3 - a failure, not a skip.
//
// Exit: 0 deadline reached, nothing found   1 a discrepancy   3 environment   4 harness fault

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

#include "aios/hal/process.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/storage/object_store.hpp"
#include "aios/userland/git.hpp"
#include "aios/userland/git_merge.hpp"
#include "aios/userland/git_remote.hpp"

using namespace aios;
using namespace aios::userland;
namespace fs = std::filesystem;

namespace {

using Ops = std::map<std::string, std::string>;

constexpr int kExitOk = 0, kExitFindings = 1, kExitEnv = 3, kExitHarness = 4;

struct Config {
  fs::path corpus, corpus_def, work, findings, ledger;
  double hours = 1.0;
  uint64_t seed = 0;
  bool have_seed = false;
  uint32_t op_timeout_s = 900;
  uint32_t variant_max_mb = 150;
  int recycle_every = 150;          // re-clone the working copy after this many iterations on a repo
  int max_findings = 10;            // stop collecting after this many in one session
  std::string aios_commit = "unknown";
  bool dirty = true;
  int inject_every = 0;
  std::string replay, stop_file;
  bool gate = false;
  double min_hours = 0;
  int min_checks = 100;
  std::string only_kind;            // run one kind only (debugging a finding class)
  std::set<std::string> skip_kinds; // kinds left out while a known finding in them is being fixed
  bool restricted() const { return !only_kind.empty() || !skip_kinds.empty(); }
  bool kind_enabled(const char* n) const { return (only_kind.empty() || only_kind == n) && !skip_kinds.count(n); }
} cfg;

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

// ---------------------------------------------------------------------------------------
// small utilities
// ---------------------------------------------------------------------------------------

std::string utc(const char* fmt = "%Y%m%dT%H%M%SZ") {
  std::time_t t = std::time(nullptr);
  std::tm g{};
#ifdef _WIN32
  gmtime_s(&g, &t);
#else
  gmtime_r(&t, &g);
#endif
  char b[64];
  std::strftime(b, sizeof b, fmt, &g);
  return b;
}
int64_t mono_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
double cpu_seconds() {
#ifdef _WIN32
  FILETIME c, e, k, u;
  GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
  auto v = [](FILETIME f) { return (double)(((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e7; };
  return v(k) + v(u);
#else
  rusage r;
  getrusage(RUSAGE_SELF, &r);
  return (double)r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6 + (double)r.ru_stime.tv_sec + r.ru_stime.tv_usec / 1e6;
#endif
}
std::string read_file(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}
void write_file(const fs::path& p, const std::string& data) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(data.data(), (std::streamsize)data.size());
}
void rm_rf(const fs::path& p) {
  std::error_code ec;
  if (!fs::exists(p, ec)) return;
  for (auto& e : fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
    std::error_code e2;
    fs::permissions(e.path(), fs::perms::owner_all, fs::perm_options::add, e2);   // git leaves packs read-only on Windows
  }
  fs::remove_all(p, ec);
  if (ec) {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    fs::remove_all(p, ec);
  }
}
std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t at = 0;
  while (true) {
    size_t i = s.find(sep, at);
    if (i == std::string::npos) { out.push_back(s.substr(at)); break; }
    out.push_back(s.substr(at, i - at));
    at = i + 1;
  }
  return out;
}
std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  for (std::string w; in >> w;) out.push_back(w);
  return out;
}
std::string trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\n')) ++i;
  return s.substr(i);
}
std::string first_line(const std::string& s) {
  auto t = trim(s);
  return t.substr(0, t.find('\n'));
}
std::string join(const std::vector<std::string>& v, const std::string& sep) {
  std::string o;
  for (size_t i = 0; i < v.size(); ++i) o += (i ? sep : "") + v[i];
  return o;
}
uint64_t fnv(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
  return h;
}
std::string hex64(uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
  return b;
}
std::string mode_str(uint32_t m) {
  char b[16];
  std::snprintf(b, sizeof b, "%06o", m);
  return b;
}
bool is_hex(const std::string& s) {
  for (char c : s) if (!std::isxdigit((unsigned char)c)) return false;
  return !s.empty();
}
std::string show(const std::string& s, size_t max = 300) {
  std::string o;
  for (char c : s) { if (c == '\n') o += "\\n"; else if (c == '\0') o += "\\0"; else o += c; }
  return o.size() > max ? o.substr(0, max) + "..." : o;
}

// ---------------------------------------------------------------------------------------
// running real git
// ---------------------------------------------------------------------------------------
// hal::run_tool is not used: on Windows it strips every '\r' from the child's output, which
// would corrupt exactly the CRLF content this soak exists to compare. spawn_process with an
// output file is byte-exact, and the startup self-check proves it before anything runs.

std::string g_git;
std::string g_git_version;
fs::path g_tmp;
std::vector<std::pair<std::string, std::string>> g_env;

struct Run {
  bool launched = false;
  int code = -1;
  std::string out;     // stdout and stderr, as the child wrote them
  std::string err;     // why it did not run at all
  bool ok() const { return launched && code == 0; }
};

Run exec(const std::vector<std::string>& argv, uint32_t timeout_ms = 600000, const std::string& cwd = {}) {
  static std::atomic<uint64_t> counter{0};
  Run r;
  fs::path outp = g_tmp / ("o" + std::to_string(counter++) + ".bin");
  std::error_code ec;
  fs::remove(outp, ec);
  hal::ProcessSpawn s;
  s.argv = argv;
  s.working_dir = cwd;
  s.env = g_env;
  s.unset_env = {"GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES",
                 "GIT_NAMESPACE", "GIT_CEILING_DIRECTORIES", "GIT_DIFF_OPTS", "GIT_EXTERNAL_DIFF", "GIT_PAGER", "PAGER"};
  s.output_path = outp.string();
  auto h = hal::spawn_process(s);
  if (!h.ok()) { r.err = h.error().str(); return r; }
  auto st = hal::wait_process(*h, timeout_ms);
  if (!st.ok()) {
    (void)hal::signal_process(*h, hal::ProcessSignal::Kill);
    (void)hal::release_process(*h);
    fs::remove(outp, ec);
    r.err = "timed out after " + std::to_string(timeout_ms) + "ms: " + argv.back();
    return r;
  }
  (void)hal::release_process(*h);
  r.launched = true;
  r.code = st->exit_code;
  r.out = read_file(outp);
  fs::remove(outp, ec);
  return r;
}

// git against one repository, with everything that could make two machines disagree pinned.
Run gitc(const std::string& gitdir, const std::vector<std::string>& args, uint32_t timeout_ms = 600000) {
  std::vector<std::string> a{g_git};
  if (!gitdir.empty()) a.push_back("--git-dir=" + gitdir);
  static const char* pins[] = {"core.autocrlf=false", "core.safecrlf=false", "core.quotepath=false", "core.longpaths=true",
                               "core.fsmonitor=false", "core.commitGraph=false", "gc.auto=0", "merge.directoryRenames=false", "merge.renameLimit=1000000",
                               "diff.renameLimit=1000000", "merge.renames=true", "diff.renames=true", "merge.conflictStyle=merge",
                               "diff.algorithm=myers", "diff.indentHeuristic=true", "diff.noprefix=false", "diff.mnemonicPrefix=false"};
  for (const char* p : pins) { a.push_back("-c"); a.push_back(p); }
  a.insert(a.end(), args.begin(), args.end());
  return exec(a, timeout_ms);
}

// ---------------------------------------------------------------------------------------
// corpus
// ---------------------------------------------------------------------------------------

struct CorpusEntry {
  std::string name, url, pin;
  std::vector<std::string> variants;      // "deep", "ref"
  fs::path master;
};
struct Commit { std::string oid; std::vector<std::string> parents; };

struct RepoState {
  CorpusEntry e;
  fs::path dir;                           // working copy: dir/.git is a bare layout
  std::string gitdir;
  fs::path fsck_dir;                      // a scratch repo that borrows the master's objects, holding only what AIOS wrote
  std::unique_ptr<Git> git;
  std::map<std::string, std::unique_ptr<Git>> vgit;   // repacked variants of the same objects, read-only
  std::map<std::string, std::string> vdir;
  std::vector<Commit> commits;
  std::unordered_map<std::string, size_t> index;
  std::vector<size_t> merges, plain;      // plain: exactly one parent
  std::vector<size_t> criss;              // merges known to have more than one merge base
  std::set<size_t> criss_tried;
  std::unordered_set<std::string> fsck_seen;
  int iterations = 0;
};

Kernel* g_kernel = nullptr;
std::vector<std::unique_ptr<RepoState>> g_repos;

std::vector<CorpusEntry> load_corpus_def(std::string* why) {
  std::vector<CorpusEntry> out;
  std::ifstream f(cfg.corpus_def);
  if (!f) { *why = "cannot read " + cfg.corpus_def.string(); return out; }
  for (std::string line; std::getline(f, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    auto c = split(line, '\t');
    if (c.size() < 3 || c[2].empty()) { *why = "corpus line without a pinned commit: " + line; return {}; }
    CorpusEntry e;
    e.name = c[0]; e.url = c[1]; e.pin = c[2];
    if (c.size() > 3 && !c[3].empty()) e.variants = split(c[3], ',');
    e.master = cfg.corpus / (e.name + ".git");
    out.push_back(e);
  }
  if (out.empty()) *why = "the corpus definition names no repository";
  return out;
}

bool open_git(const fs::path& dir, std::unique_ptr<Git>& out, std::string& why) {
  auto g = std::make_unique<Git>(*g_kernel);
  auto r = g->open(dir.string());
  if (!r.ok()) { why = "Git::open(" + dir.string() + "): " + r.error().str(); return false; }
  out = std::move(g);
  return true;
}

uint64_t pack_bytes(const fs::path& gitdir) {
  uint64_t n = 0;
  std::error_code ec;
  for (auto& f : fs::directory_iterator(gitdir / "objects" / "pack", ec))
    if (f.path().extension() == ".pack") n += f.file_size(ec);
  return n;
}

// A fresh working copy of the master: hardlinked packs, nothing of AIOS's from an earlier round.
bool prepare_worker(RepoState& rs, std::string& why) {
  rs.git.reset();
  rs.fsck_seen.clear();
  rm_rf(rs.dir);
  rm_rf(rs.fsck_dir);
  std::error_code ec;
  fs::create_directories(rs.dir, ec);
  auto c = gitc("", {"clone", "--bare", "--local", "--quiet", rs.e.master.string(), rs.gitdir});
  if (!c.ok()) { why = "clone of " + rs.e.name + ": " + trim(c.out + c.err); return false; }
  auto i = gitc("", {"init", "--bare", "--quiet", rs.fsck_dir.string()});
  if (!i.ok()) { why = "init fsck repo: " + trim(i.out + i.err); return false; }
  write_file(rs.fsck_dir / "objects" / "info" / "alternates", (rs.e.master / "objects").generic_string() + "\n");
  return open_git(rs.dir, rs.git, why);
}

bool prepare_variant(RepoState& rs, const std::string& v, std::string& why) {
  fs::path vd = cfg.work / "repos" / (rs.e.name + "-" + v);
  fs::path marker = vd / "variant.ok";
  if (!(fs::exists(marker) && read_file(marker) == rs.e.pin)) {
    rm_rf(vd);
    fs::create_directories(vd);
    std::string gd = (vd / ".git").string();
    auto c = gitc("", {"clone", "--bare", "--local", "--quiet", rs.e.master.string(), gd});
    if (!c.ok()) { why = "variant clone: " + trim(c.out + c.err); return false; }
    std::vector<std::string> a;
    if (v == "ref") { a = {"-c", "repack.useDeltaBaseOffset=false"}; }
    std::vector<std::string> args = a;
    for (auto s : {"repack", "-a", "-d", "-f", "-q"}) args.push_back(s);
    if (v == "deep") { args.push_back("--window=100"); args.push_back("--depth=250"); }
    auto r = gitc(gd, args, 3600000);
    if (!r.ok()) { why = "variant repack " + v + ": " + trim(r.out + r.err); return false; }
    write_file(marker, rs.e.pin);
  }
  std::unique_ptr<Git> g;
  if (!open_git(vd, g, why)) return false;
  rs.vdir[v] = vd.string();
  rs.vgit[v] = std::move(g);
  return true;
}

bool index_repo(RepoState& rs, std::string& why) {
  auto r = gitc(rs.gitdir, {"rev-list", "--parents", rs.e.pin});
  if (!r.ok()) { why = "rev-list " + rs.e.pin + " in " + rs.e.name + ": " + trim(r.out + r.err); return false; }
  rs.commits.clear(); rs.index.clear(); rs.merges.clear(); rs.plain.clear(); rs.criss.clear();
  size_t at = 0;
  const std::string& s = r.out;
  while (at < s.size()) {
    size_t nl = s.find('\n', at);
    if (nl == std::string::npos) nl = s.size();
    auto f = split(s.substr(at, nl - at), ' ');
    at = nl + 1;
    if (f.empty() || f[0].size() < 40) continue;
    Commit c;
    c.oid = f[0];
    for (size_t i = 1; i < f.size(); ++i) if (!f[i].empty()) c.parents.push_back(f[i]);
    rs.index[c.oid] = rs.commits.size();
    rs.commits.push_back(std::move(c));
  }
  for (size_t i = 0; i < rs.commits.size(); ++i) {
    if (rs.commits[i].parents.size() >= 2) rs.merges.push_back(i);
    else if (rs.commits[i].parents.size() == 1) rs.plain.push_back(i);
  }
  return !rs.commits.empty();
}

// ---------------------------------------------------------------------------------------
// outcomes, statistics, findings
// ---------------------------------------------------------------------------------------

struct Outcome {
  enum V { Pass, Fail, Skip } v = Pass;
  std::string aios, git, note;
};
Outcome skip(std::string why) { Outcome o; o.v = Outcome::Skip; o.note = std::move(why); return o; }

uint64_t g_compares = 0;
Outcome compare(std::string aios, const std::string& git, std::string note = {}) {
  if (cfg.inject_every && ++g_compares % (uint64_t)cfg.inject_every == 0) aios += "\n#INJECTED-FAULT\n";
  Outcome o;
  o.aios = std::move(aios);
  o.git = git;
  o.note = std::move(note);
  o.v = o.aios == o.git ? Outcome::Pass : Outcome::Fail;
  return o;
}

struct Counter { uint64_t pass = 0, fail = 0, skip = 0, nopick = 0; };
std::map<std::string, Counter> g_stats;
std::map<std::string, uint64_t> g_repo_checks, g_cov, g_skipwhy;
int g_findings_written = 0;
std::set<std::string> g_finding_keys;
std::mutex g_mx;

// what is in flight, for the crash journal and the watchdog
struct Inflight { std::string kind, repo, pin; uint64_t seed = 0; uint64_t iter = 0; Ops ops; };
Inflight g_inflight;
std::atomic<int64_t> g_op_started{0};
uint64_t g_iteration = 0, g_checks = 0;
int64_t g_start_ms = 0;
double g_cpu_start = 0;
std::string g_session;

std::string repro_text(const Inflight& f) {
  std::ostringstream o;
  o << "kind=" << f.kind << "\nrepo=" << f.repo << "\npin=" << f.pin << "\nseed=" << f.seed << "\niteration=" << f.iter
    << "\naios_commit=" << cfg.aios_commit << "\ndirty=" << (cfg.dirty ? 1 : 0) << "\ngit_version=" << g_git_version << "\n";
  for (auto& [k, v] : f.ops) o << "op." << k << "=" << v << "\n";
  return o.str();
}

// Returns the finding's directory. Never overwrites: the same operands are one finding.
std::string write_finding(const Inflight& f, const std::string& aios, const std::string& git, const std::string& note,
                          const std::string& tag = {}) {
  std::string key = f.kind + "|" + f.repo;
  for (auto& [k, v] : f.ops) key += "|" + k + "=" + v;
  if (!g_finding_keys.insert(key).second) return {};
  fs::path d = cfg.findings / (utc() + "-" + (tag.empty() ? "" : tag + "-") + f.kind + "-" + hex64(f.seed).substr(8));
  write_file(d / "repro.txt", repro_text(f));
  write_file(d / "aios.out", aios);
  write_file(d / "git.out", git);
  write_file(d / "detail.txt", note + "\n");
  write_file(d / "status", "open\n");
  ++g_findings_written;
  std::cout << "\n[FINDING] " << d.string() << "\n  kind " << f.kind << " repo " << f.repo << " seed " << f.seed
            << (note.empty() ? "" : "\n  " + note) << "\n  aios: " << show(aios) << "\n  git:  " << show(git) << "\n" << std::flush;
  return d.string();
}

void write_ledger(const char* state) {
  std::lock_guard<std::mutex> l(g_mx);
  std::ostringstream o;
  o << "session=" << g_session << "\nstate=" << state << "\nstart_utc=" << g_session.substr(0, 16) << "\nupdate_utc=" << utc()
    << "\naios_commit=" << cfg.aios_commit << "\ndirty=" << (cfg.dirty ? 1 : 0) << "\ngit_version=" << g_git_version
    << "\ninject=" << cfg.inject_every << "\nrestricted=" << (cfg.restricted() ? 1 : 0) << "\nwall_s=" << (double)(mono_ms() - g_start_ms) / 1000.0
    << "\ncpu_s=" << (cpu_seconds() - g_cpu_start) << "\niterations=" << g_iteration << "\nchecks=" << g_checks
    << "\nfindings=" << g_findings_written << "\n";
  for (auto& [k, c] : g_stats) o << "kind." << k << "=" << (c.pass + c.fail) << "\nskip." << k << "=" << c.skip << "\n";
  for (auto& [k, n] : g_repo_checks) o << "repo." << k << "=" << n << "\n";
  for (auto& [k, n] : g_cov) o << "cov." << k << "=" << n << "\n";
  for (auto& [k, n] : g_skipwhy) o << "skipwhy." << k << "=" << n << "\n";
  write_file(cfg.ledger / (g_session + ".txt"), o.str());
}

void journal_set(const Inflight& f) { write_file(cfg.work / "inflight.txt", repro_text(f)); }
void journal_clear() { std::error_code ec; fs::remove(cfg.work / "inflight.txt", ec); }

// ---------------------------------------------------------------------------------------
// helpers for the checks
// ---------------------------------------------------------------------------------------

struct Chg { std::string omode, nmode, ooid, noid, status; };

bool changed(RepoState& rs, const std::string& a, const std::string& b, std::map<std::string, Chg>& out) {
  auto r = gitc(rs.gitdir, {"diff-tree", "-r", "--no-renames", "--no-abbrev", "-z", "--raw", a, b});
  if (!r.ok()) return false;
  auto t = split(r.out, '\0');
  for (size_t i = 0; i < t.size();) {
    if (t[i].empty() || t[i][0] != ':' || i + 1 >= t.size()) { ++i; continue; }
    auto f = split(t[i].substr(1), ' ');
    if (f.size() >= 5) out[t[i + 1]] = Chg{f[0], f[1], f[2], f[3], f[4]};
    i += 2;
  }
  return true;
}

std::string back_off(RepoState& rs, const std::string& oid, uint64_t k) {
  auto r = gitc(rs.gitdir, {"rev-parse", "--verify", "-q", oid + "~" + std::to_string(k) + "^{commit}"});
  auto t = trim(r.out);
  return r.ok() && t.size() >= 40 ? t : oid;
}

std::string blob(RepoState& rs, const std::string& oid, bool* okp = nullptr) {
  auto r = gitc(rs.gitdir, {"cat-file", "blob", oid});
  if (okp) *okp = r.ok();
  return r.out;
}

GitMergeFavor favor_of(const std::string& s) {
  return s == "ours" ? GitMergeFavor::Ours : s == "theirs" ? GitMergeFavor::Theirs : s == "union" ? GitMergeFavor::Union : GitMergeFavor::None;
}

// What AIOS wrote since the last look, checked by real git's fsck --strict in a scratch repo that
// borrows the master's objects for links. Empty means clean.
std::string fsck_new_loose(RepoState& rs) {
  fs::path objs = fs::path(rs.gitdir) / "objects";
  int copied = 0;
  std::error_code ec;
  for (auto& d : fs::directory_iterator(objs, ec)) {
    std::string pre = d.path().filename().string();
    if (pre.size() != 2 || !is_hex(pre) || !d.is_directory()) continue;
    for (auto& f : fs::directory_iterator(d.path(), ec)) {
      std::string rest = f.path().filename().string();
      if (rest.size() != 38 || !is_hex(rest)) continue;
      if (!rs.fsck_seen.insert(pre + rest).second) continue;
      fs::create_directories(rs.fsck_dir / "objects" / pre, ec);
      fs::copy_file(f.path(), rs.fsck_dir / "objects" / pre / rest, fs::copy_options::skip_existing, ec);
      ++copied;
    }
  }
  if (!copied) return {};
  auto r = gitc(rs.fsck_dir.string(), {"fsck", "--strict", "--no-full", "--no-dangling", "--no-progress"});
  std::string bad;
  for (auto& l : split(r.out, '\n')) {
    if (l.empty() || l.rfind("notice:", 0) == 0 || l.rfind("Checking", 0) == 0) continue;
    bad += l + "\n";
  }
  if (!r.launched) return "fsck did not run: " + r.err;
  if (r.code != 0 || !bad.empty()) return "exit " + std::to_string(r.code) + "\n" + bad;
  return {};
}

// ---------------------------------------------------------------------------------------
// the checks. Each has a pick (choose operands from the corpus) and a run (put them to both).
// ---------------------------------------------------------------------------------------

using Rng = std::mt19937_64;
size_t pick_n(Rng& r, size_t n) { return n ? (size_t)(r() % n) : 0; }
bool chance(Rng& r, double p) { return std::uniform_real_distribution<double>(0, 1)(r) < p; }

// -- treemerge ---------------------------------------------------------------------------

bool hunt_criss(RepoState& rs, Rng& r) {
  for (int i = 0; i < 12 && !rs.merges.empty(); ++i) {
    size_t m = rs.merges[pick_n(r, rs.merges.size())];
    if (!rs.criss_tried.insert(m).second) continue;
    auto& c = rs.commits[m];
    auto b = gitc(rs.gitdir, {"merge-base", "--all", c.parents[0], c.parents[1]});
    if (b.ok() && split(trim(b.out), '\n').size() > 1) { rs.criss.push_back(m); return true; }
  }
  return !rs.criss.empty();
}

bool pick_treemerge(RepoState& rs, Rng& r, Ops& o) {
  if (rs.merges.empty()) return false;
  size_t m = rs.merges[pick_n(r, rs.merges.size())];
  if (chance(r, 0.25) && hunt_criss(rs, r)) { m = rs.criss[pick_n(r, rs.criss.size())]; o["hint"] = "criss"; }
  std::string a = rs.commits[m].parents[0], b = rs.commits[m].parents[1];
  if (o.find("hint") == o.end() && chance(r, 0.3)) { a = back_off(rs, a, 1 + pick_n(r, 6)); b = back_off(rs, b, 1 + pick_n(r, 6)); }
  if (a == b) return false;
  double p = std::uniform_real_distribution<double>(0, 1)(r);
  o["a"] = a; o["b"] = b; o["favor"] = p < 0.75 ? "none" : p < 0.875 ? "ours" : "theirs";
  return true;
}

Outcome run_treemerge(RepoState& rs, const Ops& o) {
  const std::string &a = o.at("a"), &b = o.at("b"), &fav = o.at("favor");
  std::vector<std::string> args{"merge-tree", "--write-tree", "--name-only", "--no-messages", "-z", "--allow-unrelated-histories"};
  if (fav == "ours") args.push_back("-Xours");
  else if (fav == "theirs") args.push_back("-Xtheirs");
  args.push_back(a); args.push_back(b);
  Run g = gitc(rs.gitdir, args);
  if (!g.launched || g.code > 1) return skip("git merge-tree failed: " + first_line(g.out + g.err));

  auto parts = split(g.out, '\0');
  std::string git_repr;
  if (g.code == 0) git_repr = "clean tree=" + parts[0];
  else {
    std::set<std::string> paths;
    for (size_t i = 1; i < parts.size() && !parts[i].empty(); ++i) paths.insert(parts[i]);
    git_repr = "conflict\n";
    for (auto& p : paths) git_repr += p + "\n";
  }

  GitTreeMergeOptions mo;
  mo.favor = favor_of(fav);
  auto m = git_merge_commits(*rs.git, a, b, "soak", false, mo);
  std::string aios_repr;
  if (!m.ok()) aios_repr = "error: " + m.error().str();
  else if (m->tree.clean) {
    std::string tree = m->tree.tree;
    if (tree.empty() && (m->kind == GitCommitMerge::Kind::UpToDate || m->kind == GitCommitMerge::Kind::FastForward)) {
      // no merge was needed, so the answer is the tree of the side that already contained the other
      bool up = m->kind == GitCommitMerge::Kind::UpToDate;
      auto win = rs.git->read_commit(up ? a : b);
      tree = win.ok() ? win->tree : std::string();
      ++g_cov[up ? "treemerge.up_to_date" : "treemerge.fast_forward"];
    }
    aios_repr = "clean tree=" + tree;
  } else {
    std::set<std::string> paths;
    for (auto& pc : m->tree.conflicts) paths.insert(pc.path);
    aios_repr = "conflict\n";
    for (auto& p : paths) aios_repr += p + "\n";
  }
  if (m.ok()) {
    if (!m->tree.renames_ours.empty() || !m->tree.renames_theirs.empty()) ++g_cov["treemerge.with_renames"];
    if (!m->tree.clean) ++g_cov["treemerge.conflicted"];
    if (o.count("hint")) ++g_cov["treemerge.criss_cross"];
    if (!m->tree.auto_merged.empty()) ++g_cov["treemerge.auto_merged_files"];
  }
  std::string f = fsck_new_loose(rs);
  git_repr += "\nfsck: clean";
  aios_repr += "\nfsck: " + (f.empty() ? std::string("clean") : f);
  return compare(aios_repr, git_repr);
}

// -- merge3 ------------------------------------------------------------------------------

Outcome cmp_merge3(const std::string& base, const std::string& ours, const std::string& theirs, const std::string& mode) {
  static std::atomic<uint64_t> n{0};
  fs::path d = g_tmp / ("m3" + std::to_string(n++ % 4));
  write_file(d / "o", ours); write_file(d / "b", base); write_file(d / "t", theirs);
  std::vector<std::string> args{g_git, "merge-file", "-p", "-q"};
  if (mode != "none") args.push_back("--" + mode);
  for (auto s : {"-L", "ours", "-L", "base", "-L", "theirs"}) args.push_back(s);
  args.push_back((d / "o").string()); args.push_back((d / "b").string()); args.push_back((d / "t").string());
  Run g = exec(args);
  if (!g.launched) return skip("git merge-file did not run: " + g.err);

  GitMerge3Options mo;
  mo.favor = favor_of(mode);
  auto r = git_merge3(base, ours, theirs, mo);
  int code = mode == "none" ? (int)std::min<size_t>(r.conflicts.size(), 127) : 0;
  std::string aios_repr = "exit=" + std::to_string(code) + "\n" + r.merged;
  std::string git_repr = "exit=" + std::to_string(g.code) + "\n" + g.out;
  if (base.find("\r\n") != std::string::npos || ours.find("\r\n") != std::string::npos) ++g_cov["merge3.crlf"];
  if (g.code > 0) ++g_cov["merge3.conflicted"];
  return compare(aios_repr, git_repr, "mode " + mode);
}

bool pick_merge3(RepoState& rs, Rng& r, Ops& o) {
  if (rs.merges.empty()) return false;
  for (int attempt = 0; attempt < 6; ++attempt) {
    auto& m = rs.commits[rs.merges[pick_n(r, rs.merges.size())]];
    auto mb = gitc(rs.gitdir, {"merge-base", "--all", m.parents[0], m.parents[1]});
    auto bases = split(trim(mb.out), '\n');
    if (!mb.ok() || bases.size() != 1 || bases[0].size() < 40) continue;
    std::map<std::string, Chg> ca, cb;
    if (!changed(rs, bases[0], m.parents[0], ca) || !changed(rs, bases[0], m.parents[1], cb)) continue;
    std::vector<std::string> both;
    for (auto& [p, c] : ca) {
      auto it = cb.find(p);
      if (it == cb.end() || c.status != "M" || it->second.status != "M") continue;
      if (c.nmode == "120000" || c.nmode == "160000" || it->second.nmode == "120000") continue;
      both.push_back(p);
    }
    if (both.empty()) continue;
    const std::string& p = both[pick_n(r, both.size())];
    o["base"] = ca[p].ooid; o["ours"] = ca[p].noid; o["theirs"] = cb[p].noid; o["path"] = p;
    double q = std::uniform_real_distribution<double>(0, 1)(r);
    o["mode"] = q < 0.6 ? "none" : q < 0.73 ? "ours" : q < 0.86 ? "theirs" : "union";
    return true;
  }
  return false;
}

Outcome run_merge3(RepoState& rs, const Ops& o) {
  bool ok1, ok2, ok3;
  std::string b = blob(rs, o.at("base"), &ok1), a = blob(rs, o.at("ours"), &ok2), t = blob(rs, o.at("theirs"), &ok3);
  if (!ok1 || !ok2 || !ok3) return skip("blob unreadable by git");
  if (b.find('\0') != std::string::npos || a.find('\0') != std::string::npos || t.find('\0') != std::string::npos) {
    ++g_cov["merge3.binary_skipped"];   // binary files are merged by tree merge (conflict / favor), which treemerge covers
    return skip("binary content");
  }
  return cmp_merge3(b, a, t, o.at("mode"));
}

// -- diff --------------------------------------------------------------------------------

Outcome cmp_diff(const std::string& a, const std::string& b, int ctx, RepoState* rs, const std::string& oa, const std::string& ob) {
  std::string git_hunks;
  if (rs) {
    Run g = gitc(rs->gitdir, {"diff", "--no-color", "--no-ext-diff", "--no-textconv", "--indent-heuristic", "--diff-algorithm=myers",
                              "-U" + std::to_string(ctx), oa, ob});
    if (!g.launched || g.code > 1) return skip("git diff failed: " + first_line(g.out + g.err));
    size_t at = g.out.find("\n@@");
    git_hunks = g.out.compare(0, 2, "@@") == 0 ? g.out : (at == std::string::npos ? "" : g.out.substr(at + 1));
  } else {
    static std::atomic<uint64_t> n{0};
    fs::path d = g_tmp / ("df" + std::to_string(n++ % 4));
    write_file(d / "a", a); write_file(d / "b", b);
    Run g = exec({g_git, "diff", "--no-index", "--no-color", "--no-ext-diff", "--indent-heuristic", "--diff-algorithm=myers",
                  "-U" + std::to_string(ctx), (d / "a").string(), (d / "b").string()});
    if (!g.launched || g.code > 1) return skip("git diff failed: " + first_line(g.out + g.err));
    size_t at = g.out.find("@@");
    git_hunks = at == std::string::npos ? "" : g.out.substr(at);
  }
  if (a.find("\r\n") != std::string::npos) ++g_cov["diff.crlf"];
  return compare(git_unified_hunks(a, b, ctx), git_hunks, "context " + std::to_string(ctx));
}

bool pick_diff(RepoState& rs, Rng& r, Ops& o) {
  if (rs.plain.empty()) return false;
  for (int attempt = 0; attempt < 6; ++attempt) {
    auto& c = rs.commits[rs.plain[pick_n(r, rs.plain.size())]];
    std::map<std::string, Chg> ch;
    if (!changed(rs, c.parents[0], c.oid, ch)) continue;
    std::vector<std::string> mods;
    for (auto& [p, x] : ch) if (x.status == "M" && x.nmode != "120000" && x.nmode != "160000") mods.push_back(p);
    if (mods.empty()) continue;
    const std::string& p = mods[pick_n(r, mods.size())];
    static const int ctxs[] = {0, 1, 3, 3, 3, 5, 10};
    o["old"] = ch[p].ooid; o["new"] = ch[p].noid; o["path"] = p; o["ctx"] = std::to_string(ctxs[pick_n(r, 7)]);
    return true;
  }
  return false;
}

Outcome run_diff(RepoState& rs, const Ops& o) {
  bool ok1, ok2;
  std::string a = blob(rs, o.at("old"), &ok1), b = blob(rs, o.at("new"), &ok2);
  if (!ok1 || !ok2) return skip("blob unreadable by git");
  if (a.find('\0') != std::string::npos || b.find('\0') != std::string::npos) { ++g_cov["diff.binary_skipped"]; return skip("binary content"); }
  return cmp_diff(a, b, std::atoi(o.at("ctx").c_str()), &rs, o.at("old"), o.at("new"));
}

// -- mergebase ---------------------------------------------------------------------------

bool pick_mergebase(RepoState& rs, Rng& r, Ops& o) {
  if (rs.commits.size() < 2) return false;
  std::string a, b;
  double q = std::uniform_real_distribution<double>(0, 1)(r);
  if (q < 0.4 && !rs.merges.empty()) {
    auto& m = rs.commits[rs.merges[pick_n(r, rs.merges.size())]];
    a = m.parents[0]; b = m.parents[1];
  } else if (q < 0.7) {
    a = rs.commits[pick_n(r, rs.commits.size())].oid;
    b = back_off(rs, a, 1 + pick_n(r, 60));
    if (chance(r, 0.5)) std::swap(a, b);
  } else {
    a = rs.commits[pick_n(r, rs.commits.size())].oid;
    b = rs.commits[pick_n(r, rs.commits.size())].oid;
  }
  o["a"] = a; o["b"] = b;
  return true;
}

Outcome run_mergebase(RepoState& rs, const Ops& o) {
  const std::string &a = o.at("a"), &b = o.at("b");
  Run g = gitc(rs.gitdir, {"merge-base", "--all", a, b});
  Run anc = gitc(rs.gitdir, {"merge-base", "--is-ancestor", a, b});
  if (!g.launched || g.code > 1 || !anc.launched || anc.code > 1) return skip("git merge-base failed: " + first_line(g.out + g.err));
  std::vector<std::string> want;
  for (auto& l : split(trim(g.out), '\n')) if (!l.empty()) want.push_back(l);
  std::sort(want.begin(), want.end());
  std::string git_repr = "bases:\n" + join(want, "\n") + "\nancestor(a,b)=" + std::to_string(anc.code == 0);
  if (want.size() > 1) ++g_cov["mergebase.multiple"];

  std::string aios_repr;
  auto got = git_merge_bases(*rs.git, a, b);
  auto isanc = git_is_ancestor(*rs.git, a, b);
  if (!got.ok()) aios_repr = "error: " + got.error().str();
  else if (!isanc.ok()) aios_repr = "error: " + isanc.error().str();
  else {
    auto v = *got;
    std::sort(v.begin(), v.end());
    aios_repr = "bases:\n" + join(v, "\n") + "\nancestor(a,b)=" + std::to_string((int)*isanc);
  }
  return compare(aios_repr, git_repr);
}

// -- objects -----------------------------------------------------------------------------

bool pick_objects(RepoState& rs, Rng& r, Ops& o) {
  if (rs.commits.empty()) return false;
  std::vector<std::string> vs{"main"};
  for (auto& [k, g] : rs.vgit) vs.push_back(k);
  o["variant"] = vs[pick_n(r, vs.size())];
  o["commit"] = rs.commits[pick_n(r, rs.commits.size())].oid;
  o["pick"] = std::to_string(r() % 1000000);
  return true;
}

Outcome run_objects(RepoState& rs, const Ops& o) {
  Git& g = o.at("variant") == "main" ? *rs.git : *rs.vgit.at(o.at("variant"));
  const std::string& c = o.at("commit");
  std::ostringstream A, G;

  // the commit object, byte for byte
  Run gc = gitc(rs.gitdir, {"cat-file", "commit", c});
  if (!gc.ok()) return skip("git cannot read the commit");
  auto ac = g.read_object(c);
  A << "commit " << (ac.ok() ? ac->type + " " + hex64(fnv(std::string(ac->payload.begin(), ac->payload.end()))) : "error: " + ac.error().str()) << "\n";
  G << "commit commit " << hex64(fnv(gc.out)) << "\n";

  auto cm = g.read_commit(c);
  if (!cm.ok()) { A << "read_commit error: " << cm.error().str() << "\n"; return compare(A.str(), G.str()); }
  Run gt = gitc(rs.gitdir, {"cat-file", "tree", cm->tree});
  auto at = g.read_object(cm->tree);
  A << "tree " << (at.ok() ? hex64(fnv(std::string(at->payload.begin(), at->payload.end()))) : "error: " + at.error().str()) << "\n";
  G << "tree " << hex64(fnv(gt.out)) << "\n";

  // every file the tree holds: path, mode, id
  Run ls = gitc(rs.gitdir, {"ls-tree", "-r", "-z", "--full-tree", c});
  if (!ls.ok()) return skip("git ls-tree failed");
  std::vector<std::string> gl;
  struct Ent { std::string mode, oid, path; };
  std::vector<Ent> ents;
  for (auto& rec : split(ls.out, '\0')) {
    if (rec.empty()) continue;
    size_t tab = rec.find('\t');
    if (tab == std::string::npos) continue;
    auto f = split(rec.substr(0, tab), ' ');
    if (f.size() < 3) continue;
    ents.push_back({f[0], f[2], rec.substr(tab + 1)});
    gl.push_back(f[0] + " " + f[2] + " " + rec.substr(tab + 1));
  }
  std::sort(gl.begin(), gl.end());
  std::vector<GitIndexEntry> flat;
  auto fr = g.flatten_tree(cm->tree, "", flat);
  std::vector<std::string> al;
  if (!fr.ok()) al.push_back("flatten error: " + fr.error().str());
  else for (auto& e : flat) al.push_back(mode_str(e.mode) + " " + e.oid + " " + e.path);
  std::sort(al.begin(), al.end());
  A << "files " << al.size() << "\n" << join(al, "\n") << "\n";
  G << "files " << gl.size() << "\n" << join(gl, "\n") << "\n";

  // a few of the blobs, read through the pack (deltas included)
  std::mt19937_64 br(std::strtoull(o.at("pick").c_str(), nullptr, 10));
  for (int i = 0; i < 4 && !ents.empty(); ++i) {
    const Ent& e = ents[pick_n(br, ents.size())];
    if (e.mode == "160000") continue;
    Run gb = gitc(rs.gitdir, {"cat-file", "blob", e.oid});
    auto ab = g.read_object(e.oid);
    A << "blob " << e.oid << " " << (ab.ok() ? ab->type + " " + std::to_string(ab->payload.size()) + " " + hex64(fnv(std::string(ab->payload.begin(), ab->payload.end()))) : "error: " + ab.error().str()) << "\n";
    G << "blob " << e.oid << " " << (gb.ok() ? "blob " + std::to_string(gb.out.size()) + " " + hex64(fnv(gb.out)) : "error") << "\n";
  }
  ++g_cov["objects." + o.at("variant")];
  return compare(A.str(), G.str(), "variant " + o.at("variant"));
}

// -- renames -----------------------------------------------------------------------------

bool pick_renames(RepoState& rs, Rng& r, Ops& o) {
  if (rs.plain.empty()) return false;
  for (int attempt = 0; attempt < 8; ++attempt) {
    auto& c = rs.commits[rs.plain[pick_n(r, rs.plain.size())]];
    auto n = gitc(rs.gitdir, {"diff-tree", "-r", "--name-only", "-z", c.parents[0], c.oid});
    if (!n.ok()) continue;
    size_t count = split(n.out, '\0').size();
    if (count < 2 || count > 400) continue;
    o["parent"] = c.parents[0]; o["commit"] = c.oid;
    return true;
  }
  return false;
}

Outcome run_renames(RepoState& rs, const Ops& o) {
  const std::string &p = o.at("parent"), &c = o.at("commit");
  Run g = gitc(rs.gitdir, {"diff-tree", "-r", "-M", "-l0", "--name-status", "-z", p, c});
  if (!g.ok()) return skip("git diff-tree failed");
  std::set<std::string> want;
  auto t = split(g.out, '\0');
  for (size_t i = 0; i < t.size(); ++i) {
    if (t[i].empty()) continue;
    char s = t[i][0];
    if (s == 'R' || s == 'C') {
      if (i + 2 >= t.size()) break;
      if (s == 'R') want.insert(t[i + 1] + " -> " + t[i + 2] + " " + std::to_string(std::atoi(t[i].c_str() + 1)));
      i += 2;
    } else ++i;
  }
  auto pc = rs.git->read_commit(p), cc = rs.git->read_commit(c);
  std::string aios_repr;
  std::string git_repr;
  for (auto& x : want) git_repr += x + "\n";
  if (!pc.ok() || !cc.ok()) aios_repr = "error: cannot read commit";
  else {
    auto rr = git_detect_renames(*rs.git, pc->tree, cc->tree);
    if (!rr.ok()) aios_repr = "error: " + rr.error().str();
    else {
      std::set<std::string> got;
      for (auto& x : *rr) got.insert(x.from + " -> " + x.to + " " + std::to_string(x.percent));
      for (auto& x : got) aios_repr += x + "\n";
    }
  }
  if (!want.empty()) ++g_cov["renames.present"];
  return compare(aios_repr, git_repr);
}

// -- packwrite ---------------------------------------------------------------------------

bool pick_packwrite(RepoState& rs, Rng& r, Ops& o) {
  if (rs.commits.empty()) return false;
  o["commit"] = rs.commits[pick_n(r, rs.commits.size())].oid;
  o["k"] = std::to_string(1 + pick_n(r, 25));
  return true;
}

Outcome run_packwrite(RepoState& rs, const Ops& o) {
  const std::string& c = o.at("commit");
  uint64_t k = std::strtoull(o.at("k").c_str(), nullptr, 10);
  std::string base = back_off(rs, c, k);
  Run rl = base == c ? gitc(rs.gitdir, {"rev-list", "--objects", "-n", std::to_string(k), c})
                     : gitc(rs.gitdir, {"rev-list", "--objects", c, "^" + base});
  if (!rl.ok()) return skip("git rev-list --objects failed");
  std::vector<std::string> oids;
  for (auto& l : split(rl.out, '\n')) if (l.size() >= 40) oids.push_back(l.substr(0, 40));
  if (oids.empty() || oids.size() > 20000) return skip("object count out of range");

  uint64_t counted = 0;
  auto pk = git_build_pack(*rs.git, oids, counted);
  if (!pk.ok()) return compare("pack error: " + pk.error().str(), "a pack");
  static std::atomic<uint64_t> n{0};
  fs::path d = g_tmp / ("pk" + std::to_string(n++ % 4));
  rm_rf(d);
  fs::create_directories(d);
  fs::path packf = d / "x.pack";
  write_file(packf, std::string(pk->begin(), pk->end()));
  // Strict, but not about what the corpus's own history holds: redis has trees git itself wrote with zero-padded
  // modes, and a pack of them that git builds is refused by plain --strict exactly the same way.
  Run ix = gitc(rs.gitdir, {"index-pack", "--strict=zeroPaddedFilemode=ignore", packf.string()});
  std::vector<std::string> want, got;
  std::string git_repr;
  if (!ix.ok()) git_repr = "index-pack rejected: " + trim(ix.out);
  else {
    Run vp = gitc(rs.gitdir, {"verify-pack", "-v", (d / "x.idx").string()});
    for (auto& l : split(vp.out, '\n')) {
      auto f = split_ws(l);
      if (f.size() >= 5 && f[0].size() == 40 && is_hex(f[0]) && (f[1] == "commit" || f[1] == "tree" || f[1] == "blob" || f[1] == "tag"))
        // verify-pack's size is the delta's for a deltified entry, not the object's: id and type are compared
        // (index-pack --strict has already hashed every object back to its id).
        want.push_back(f[0] + " " + f[1]);
    }
    std::sort(want.begin(), want.end());
    git_repr = join(want, "\n");
  }
  for (auto& id : oids) {
    auto ob = rs.git->read_object(id);
    got.push_back(id + " " + (ob.ok() ? ob->type : std::string("unreadable")));
  }
  std::sort(got.begin(), got.end());
  got.erase(std::unique(got.begin(), got.end()), got.end());
  ++g_cov["packwrite.objects"];
  return compare(join(got, "\n"), git_repr, std::to_string(oids.size()) + " objects");
}

// -- synthetic generators, under fresh seeds ----------------------------------------------

struct Triple { std::string base, ours, theirs; };
Triple unique_triple(Rng& rng, const std::string& tag) {
  size_t m = 1 + pick_n(rng, 40);
  std::vector<char> in_base(m);
  for (auto& b : in_base) b = chance(rng, 0.75);
  double keep = 0.6 + 0.35 * std::uniform_real_distribution<double>(0, 1)(rng);
  auto side = [&]() {
    std::string s;
    for (size_t i = 0; i < m; ++i)
      if (in_base[i] ? chance(rng, keep) : chance(rng, 0.15)) s += tag + std::to_string(i) + "\n";
    return s;
  };
  Triple t;
  for (size_t i = 0; i < m; ++i) if (in_base[i]) t.base += tag + std::to_string(i) + "\n";
  t.ours = side();
  t.theirs = side();
  if (chance(rng, 0.1)) t.theirs = t.ours;
  if (chance(rng, 0.1)) t.ours = t.base;
  return t;
}

bool pick_synth(RepoState&, Rng& r, Ops& o) { o["gen"] = std::to_string(r()); return true; }
Outcome run_synth_merge3(RepoState&, const Ops& o) {
  Rng rng(std::strtoull(o.at("gen").c_str(), nullptr, 10));
  Triple t = unique_triple(rng, chance(rng, 0.5) ? "L" : " line ");
  static const char* modes[] = {"none", "none", "none", "ours", "theirs", "union"};
  return cmp_merge3(t.base, t.ours, t.theirs, modes[pick_n(rng, 6)]);
}
Outcome run_synth_diff(RepoState&, const Ops& o) {
  Rng rng(std::strtoull(o.at("gen").c_str(), nullptr, 10));
  Triple t = unique_triple(rng, chance(rng, 0.5) ? "fn_" : "  x");
  if (chance(rng, 0.15) && !t.ours.empty()) t.ours.pop_back();
  if (chance(rng, 0.15) && !t.base.empty()) t.base.pop_back();
  static const int ctxs[] = {0, 1, 3, 3, 5};
  return cmp_diff(t.base, t.ours, ctxs[pick_n(rng, 5)], nullptr, {}, {});
}

struct Kind {
  const char* name;
  int weight;
  bool (*pick)(RepoState&, Rng&, Ops&);
  Outcome (*run)(RepoState&, const Ops&);
};
const Kind kKinds[] = {
    {"treemerge", 26, pick_treemerge, run_treemerge}, {"merge3", 18, pick_merge3, run_merge3},
    {"diff", 14, pick_diff, run_diff},                {"mergebase", 12, pick_mergebase, run_mergebase},
    {"objects", 14, pick_objects, run_objects},       {"renames", 8, pick_renames, run_renames},
    {"packwrite", 4, pick_packwrite, run_packwrite},  {"synth_merge3", 2, pick_synth, run_synth_merge3},
    {"synth_diff", 2, pick_synth, run_synth_diff},
};
const Kind* find_kind(const std::string& n) {
  for (auto& k : kKinds) if (n == k.name) return &k;
  return nullptr;
}

// ---------------------------------------------------------------------------------------
// startup checks
// ---------------------------------------------------------------------------------------

[[noreturn]] void die(int code, const std::string& why) {
  std::cout << "[soak] " << (code == kExitEnv ? "ENVIRONMENT" : "HARNESS") << " FAILURE: " << why << "\n" << std::flush;
  std::exit(code);
}

// The harness has to move bytes faithfully, or every comparison on CRLF content is meaningless.
void byte_fidelity_check() {
  fs::path d = g_tmp / "fidelity.git";
  rm_rf(d);
  auto i = gitc("", {"init", "--bare", "--quiet", d.string()});
  if (!i.ok()) die(kExitHarness, "git init for the fidelity check: " + trim(i.out + i.err));
  std::string payload;
  for (int c = 0; c < 256; ++c) payload += (char)c;
  payload += "line one\r\nline two\r\n\rlone cr\r\n\x1a" "ctrl-z\n\n\n";
  for (int k = 0; k < 3000; ++k) payload += "padding \r\n of mixed endings \n";
  write_file(g_tmp / "fid.bin", payload);
  auto h = gitc(d.string(), {"hash-object", "-w", "--no-filters", (g_tmp / "fid.bin").string()});
  std::string oid = trim(h.out);
  if (!h.ok() || oid.size() < 40) die(kExitHarness, "git hash-object failed: " + trim(h.out));
  auto c = gitc(d.string(), {"cat-file", "blob", oid});
  if (!c.ok() || c.out != payload)
    die(kExitHarness, "bytes do not survive the trip through the process layer (sent " + std::to_string(payload.size()) + ", got " +
                          std::to_string(c.out.size()) + ")");
  rm_rf(d);
}

void init_environment() {
  std::error_code ec;
  fs::create_directories(cfg.work, ec);
  fs::create_directories(cfg.findings, ec);
  fs::create_directories(cfg.ledger, ec);
  g_tmp = cfg.work / "tmp";
  rm_rf(g_tmp);
  fs::create_directories(g_tmp, ec);
  fs::path home = cfg.work / "home";
  fs::create_directories(home, ec);
  write_file(cfg.work / "empty-gitconfig", "");
  g_env = {{"HOME", home.string()}, {"USERPROFILE", home.string()}, {"XDG_CONFIG_HOME", home.string()},
           {"GIT_CONFIG_NOSYSTEM", "1"}, {"GIT_CONFIG_GLOBAL", (cfg.work / "empty-gitconfig").string()},
           {"GIT_TERMINAL_PROMPT", "0"}, {"GIT_OPTIONAL_LOCKS", "0"}, {"LC_ALL", "C"}, {"GIT_PAGER", "cat"},
           {"GIT_AUTHOR_NAME", "soak"}, {"GIT_AUTHOR_EMAIL", "soak@aios"}, {"GIT_COMMITTER_NAME", "soak"},
           {"GIT_COMMITTER_EMAIL", "soak@aios"}};
  g_git = hal::find_program({"git"});
  if (g_git.empty()) die(kExitEnv, "git is not on PATH: there is nothing to compare against");
  Run v = exec({g_git, "--version"}, 30000);
  if (!v.ok()) die(kExitEnv, "git --version failed: " + v.err + trim(v.out));
  g_git_version = trim(v.out);
  fs::path pinfile = cfg.corpus / "GIT_VERSION";
  if (!fs::exists(pinfile)) die(kExitEnv, "no " + pinfile.string() + ": run the corpus fetch, which records the git it was baselined on");
  std::string want = trim(read_file(pinfile));
  if (want != g_git_version)
    die(kExitEnv, "this is '" + g_git_version + "' but the corpus was baselined on '" + want +
                      "'. A different git can legitimately answer differently; review, then update GIT_VERSION deliberately.");
  byte_fidelity_check();
}

std::unique_ptr<Kernel> g_kernel_owner;
void boot_kernel() {
  fs::path root = cfg.work / "kernel";
  rm_rf(root);
  fs::create_directories(root);
  KernelConfig kcfg;
  kcfg.root = root.string();
  kcfg.residency = test_config;
  auto k = Kernel::boot(kcfg);
  if (!k.ok()) die(kExitHarness, "kernel boot: " + k.error().str());
  g_kernel_owner = std::move(*k);
  g_kernel = g_kernel_owner.get();
}

void load_repos(bool all, const std::string& only) {
  std::string why;
  auto defs = load_corpus_def(&why);
  if (defs.empty()) die(kExitEnv, why);
  for (auto& e : defs) {
    if (!all && e.name != only) continue;
    if (!fs::exists(e.master)) die(kExitEnv, "corpus repository missing: " + e.master.string() + " (run the corpus fetch)");
    auto rs = std::make_unique<RepoState>();
    rs->e = e;
    auto chk = gitc(e.master.string(), {"cat-file", "-e", e.pin + "^{commit}"});
    if (!chk.ok()) die(kExitEnv, e.name + ": pinned commit " + e.pin + " is not in the mirror");
    rs->dir = cfg.work / "repos" / e.name;
    rs->gitdir = (rs->dir / ".git").string();
    rs->fsck_dir = cfg.work / "repos" / (e.name + ".fsck.git");
    std::cout << "[soak] preparing " << e.name << " @ " << e.pin.substr(0, 10) << std::flush;
    if (!prepare_worker(*rs, why)) die(kExitEnv, why);
    if (!index_repo(*rs, why)) die(kExitEnv, why.empty() ? e.name + ": no commits" : why);
    std::cout << ": " << rs->commits.size() << " commits, " << rs->merges.size() << " merges";
    if (pack_bytes(e.master) <= (uint64_t)cfg.variant_max_mb * 1024 * 1024) {
      for (auto& v : e.variants) {
        std::cout << ", variant " << v << std::flush;
        if (!prepare_variant(*rs, v, why)) die(kExitEnv, why);
      }
    } else if (!e.variants.empty()) {
      std::cout << ", variants skipped (pack larger than " << cfg.variant_max_mb << " MB)";
    }
    std::cout << "\n" << std::flush;
    g_repos.push_back(std::move(rs));
  }
  if (g_repos.empty()) die(kExitEnv, "the corpus is empty");
}

// ---------------------------------------------------------------------------------------
// iteration, watchdog, main loop
// ---------------------------------------------------------------------------------------

Outcome run_guarded(const Kind& k, RepoState& rs, const Ops& ops) {
  return k.run(rs, ops);
}

void watchdog() {
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    int64_t t0 = g_op_started.load();
    if (!t0) continue;
    if (mono_ms() - t0 > (int64_t)cfg.op_timeout_s * 1000) {
      Inflight f;
      { std::lock_guard<std::mutex> l(g_mx); f = g_inflight; }
      write_finding(f, "(still running)", "", "no answer after " + std::to_string(cfg.op_timeout_s) + "s: a hang is a discrepancy", "hang");
      journal_clear();
      write_ledger("hung");
      std::cout << "[soak] hung operation; exiting\n" << std::flush;
      std::_Exit(kExitFindings);
    }
  }
}

void recover_crash() {
  fs::path j = cfg.work / "inflight.txt";
  if (!fs::exists(j)) return;
  Inflight f;
  std::istringstream in(read_file(j));
  for (std::string l; std::getline(in, l);) {
    auto eq = l.find('=');
    if (eq == std::string::npos) continue;
    std::string k = l.substr(0, eq), v = l.substr(eq + 1);
    if (k == "kind") f.kind = v; else if (k == "repo") f.repo = v; else if (k == "pin") f.pin = v;
    else if (k == "seed") f.seed = std::strtoull(v.c_str(), nullptr, 10);
    else if (k == "iteration") f.iter = std::strtoull(v.c_str(), nullptr, 10);
    else if (k.rfind("op.", 0) == 0) f.ops[k.substr(3)] = v;
  }
  write_finding(f, "(the process died during this operation)", "", "the previous run did not finish this operation: crash, kill, or power loss. Replay it; if it completes, resolve it.", "crash");
  journal_clear();
}

RepoState* repo_named(const std::string& n) {
  for (auto& r : g_repos) if (r->e.name == n) return r.get();
  return nullptr;
}

void record(const Kind& k, RepoState& rs, const Outcome& o, const Inflight& f) {
  auto& c = g_stats[k.name];
  if (o.v == Outcome::Skip) { ++c.skip; ++g_skipwhy[std::string(k.name) + ":" + o.note.substr(0, 40)]; return; }
  ++g_checks;
  ++g_repo_checks[rs.e.name];
  if (o.v == Outcome::Pass) { ++c.pass; return; }
  ++c.fail;
  write_finding(f, o.aios, o.git, o.note);
}

void print_stats(double elapsed_h) {
  std::cout << "[soak] " << utc("%H:%M:%S") << "  " << elapsed_h << "h  iter " << g_iteration << "  checks " << g_checks << "  findings "
            << g_findings_written << "\n";
  for (auto& [k, c] : g_stats)
    std::cout << "         " << k << ": pass " << c.pass << " fail " << c.fail << " skip " << c.skip << "\n";
  std::cout << std::flush;
}

int replay_main(const std::string& dirarg) {
  fs::path d = dirarg;
  if (!fs::exists(d / "repro.txt")) die(kExitEnv, "no repro.txt in " + d.string());
  Inflight f;
  std::istringstream in(read_file(d / "repro.txt"));
  for (std::string l; std::getline(in, l);) {
    auto eq = l.find('=');
    if (eq == std::string::npos) continue;
    std::string k = l.substr(0, eq), v = l.substr(eq + 1);
    if (k == "kind") f.kind = v; else if (k == "repo") f.repo = v; else if (k == "pin") f.pin = v;
    else if (k == "seed") f.seed = std::strtoull(v.c_str(), nullptr, 10);
    else if (k.rfind("op.", 0) == 0) f.ops[k.substr(3)] = v;
  }
  const Kind* k = find_kind(f.kind);
  if (!k) die(kExitEnv, "unknown kind " + f.kind);
  init_environment();
  boot_kernel();
  load_repos(false, f.repo);
  RepoState* rs = repo_named(f.repo);
  if (!rs) die(kExitEnv, "repository " + f.repo + " is not in the corpus definition");
  if (rs->e.pin != f.pin) die(kExitEnv, "the corpus pin for " + f.repo + " moved: replaying against " + rs->e.pin + ", finding was at " + f.pin);
  journal_clear();
  Outcome o = run_guarded(*k, *rs, f.ops);
  if (o.v == Outcome::Skip) { std::cout << "[replay] SKIPPED (" << o.note << ") - not a pass; the finding stays open\n"; return kExitFindings; }
  std::cout << "[replay] " << (o.v == Outcome::Pass ? "PASS" : "STILL FAILS") << "\n  aios: " << show(o.aios, 600) << "\n  git:  " << show(o.git, 600) << "\n";
  if (o.v == Outcome::Pass) {
    write_file(d / "status", "resolved\n");
    write_file(d / "resolved.txt", "resolved_utc=" + utc() + "\naios_commit=" + cfg.aios_commit + "\ndirty=" + (cfg.dirty ? "1" : "0") + "\n");
    return kExitOk;
  }
  write_file(d / "aios.out", o.aios); write_file(d / "git.out", o.git);
  return kExitFindings;
}

std::map<std::string, std::string> read_kv(const fs::path& p) {
  std::map<std::string, std::string> m;
  std::istringstream in(read_file(p));
  for (std::string l; std::getline(in, l);) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    auto eq = l.find('=');
    if (eq != std::string::npos) m[l.substr(0, eq)] = l.substr(eq + 1);
  }
  return m;
}

// The release gate: no open finding, enough soak hours at exactly this commit on a clean tree,
// every kind and every corpus repository exercised, and proof that a fault would have been caught.
int gate_main() {
  bool ok = true;
  auto fail = [&](const std::string& s) { std::cout << "[gate] BLOCKED: " << s << "\n"; ok = false; };
  std::error_code ec;
  int open = 0;
  if (fs::exists(cfg.findings))
    for (auto& d : fs::directory_iterator(cfg.findings, ec)) {
      if (!d.is_directory()) continue;
      std::string st = trim(read_file(d.path() / "status"));
      if (st != "resolved") { ++open; fail("open finding " + d.path().filename().string()); }
    }
  if (cfg.dirty) fail("the working tree is dirty (or its commit is unknown): a soak is evidence about a commit, not about edits");
  double hours = 0;
  std::map<std::string, uint64_t> kinds, repos;
  int sessions = 0;
  if (fs::exists(cfg.ledger))
    for (auto& f : fs::directory_iterator(cfg.ledger, ec)) {
      if (f.path().extension() != ".txt" || f.path().filename().string().rfind("inject-proof", 0) == 0) continue;
      auto kv = read_kv(f.path());
      if (kv["aios_commit"] != cfg.aios_commit || kv["dirty"] != "0" || kv["inject"] != "0" || kv["restricted"] != "0") continue;
      if (kv["git_version"].empty()) continue;
      ++sessions;
      hours += std::atof(kv["wall_s"].c_str()) / 3600.0;
      for (auto& [k, v] : kv) {
        if (k.rfind("kind.", 0) == 0) kinds[k.substr(5)] += std::strtoull(v.c_str(), nullptr, 10);
        if (k.rfind("repo.", 0) == 0) repos[k.substr(5)] += std::strtoull(v.c_str(), nullptr, 10);
      }
    }
  std::cout << "[gate] commit " << cfg.aios_commit << ": " << sessions << " clean session(s), " << hours << " soak hours (need " << cfg.min_hours
            << "), " << open << " open finding(s)\n";
  if (hours < cfg.min_hours) fail("only " + std::to_string(hours) + " soak hours at this commit; " + std::to_string(cfg.min_hours) + " required");
  for (auto& k : kKinds) {
    uint64_t n = kinds[k.name];
    std::cout << "[gate]   " << k.name << ": " << n << " checks\n";
    if ((int)n < cfg.min_checks) fail(std::string(k.name) + " has " + std::to_string(n) + " checks at this commit; " + std::to_string(cfg.min_checks) + " required");
  }
  std::string defwhy;
  for (auto& e : load_corpus_def(&defwhy)) {
    uint64_t n = repos[e.name];
    std::cout << "[gate]   corpus " << e.name << ": " << n << " checks\n";
    if ((int)n < cfg.min_checks) fail("corpus repository " + e.name + " has " + std::to_string(n) + " checks; " + std::to_string(cfg.min_checks) + " required");
  }
  if (!defwhy.empty()) fail(defwhy);
  if (!fs::exists(cfg.ledger / ("inject-proof-" + cfg.aios_commit + ".txt")))
    fail("no proof that the gate bites at this commit: run once with --inject-fault 20 and see it exit 1");
  std::cout << (ok ? "[gate] OPEN: release may proceed\n" : "[gate] release refused\n");
  return ok ? kExitOk : kExitFindings;
}

}  // namespace

int main(int argc, char** argv) {
  // aios-git-soak --diff-files OLD NEW CTX: the kernel's hunks for two files, to hold beside
  // `git diff --no-index -U<CTX>` while working out a finding by hand.
  if (argc == 5 && std::string(argv[1]) == "--diff-files") {
    std::string out = git_unified_hunks(read_file(argv[2]), read_file(argv[3]), std::atoi(argv[4]));
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);   // the CRT would otherwise write every "\n" as "\r\n"
#endif
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
  }
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { if (i + 1 >= argc) die(kExitEnv, a + " needs a value"); return argv[++i]; };
    if (a == "--corpus") cfg.corpus = next();
    else if (a == "--corpus-def") cfg.corpus_def = next();
    else if (a == "--work") cfg.work = next();
    else if (a == "--findings") cfg.findings = next();
    else if (a == "--ledger") cfg.ledger = next();
    else if (a == "--hours") cfg.hours = std::atof(next().c_str());
    else if (a == "--seed") { cfg.seed = std::strtoull(next().c_str(), nullptr, 10); cfg.have_seed = true; }
    else if (a == "--op-timeout") cfg.op_timeout_s = (uint32_t)std::atoi(next().c_str());
    else if (a == "--variant-max-mb") cfg.variant_max_mb = (uint32_t)std::atoi(next().c_str());
    else if (a == "--recycle-every") cfg.recycle_every = std::atoi(next().c_str());
    else if (a == "--max-findings") cfg.max_findings = std::atoi(next().c_str());
    else if (a == "--aios-commit") cfg.aios_commit = next();
    else if (a == "--dirty") cfg.dirty = next() != "0";
    else if (a == "--inject-fault") cfg.inject_every = std::atoi(next().c_str());
    else if (a == "--replay") cfg.replay = next();
    else if (a == "--stop-file") cfg.stop_file = next();
    else if (a == "--only") cfg.only_kind = next();
    else if (a == "--skip-kind") cfg.skip_kinds.insert(next());
    else if (a == "--gate") cfg.gate = true;
    else if (a == "--min-hours") cfg.min_hours = std::atof(next().c_str());
    else if (a == "--min-checks") cfg.min_checks = std::atoi(next().c_str());
    else die(kExitEnv, "unknown argument " + a);
  }
  if (cfg.corpus.empty() || cfg.corpus_def.empty() || cfg.work.empty() || cfg.findings.empty() || cfg.ledger.empty())
    die(kExitEnv, "--corpus, --corpus-def, --work, --findings and --ledger are all required");
  if (cfg.gate) return gate_main();
  if (!cfg.replay.empty()) return replay_main(cfg.replay);

  if (cfg.inject_every) cfg.findings = cfg.findings.parent_path() / (cfg.findings.filename().string() + "-inject");
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  g_session = utc() + "-" + hex64(fnv(std::to_string(mono_ms()) + cfg.aios_commit)).substr(0, 6);
  g_start_ms = mono_ms();
  g_cpu_start = cpu_seconds();

  init_environment();
  boot_kernel();
  std::cout << "[soak] " << g_git_version << ", session " << g_session << ", commit " << cfg.aios_commit << (cfg.dirty ? " (DIRTY: will not count for a release)" : "")
            << (cfg.inject_every ? ", FAULT INJECTION every " + std::to_string(cfg.inject_every) : "") << "\n";
  recover_crash();
  load_repos(true, {});
  g_start_ms = mono_ms();            // setup (mirror clones, repacks) is not soak time
  g_cpu_start = cpu_seconds();

  std::random_device rd;
  uint64_t master = cfg.have_seed ? cfg.seed : (((uint64_t)rd() << 32) ^ rd() ^ (uint64_t)mono_ms());
  std::cout << "[soak] master seed " << master << " (each iteration's seed is derived from it and logged in any finding)\n" << std::flush;
  Rng mrng(master);

  std::thread(watchdog).detach();
  write_ledger("running");

  int total_w = 0;
  for (auto& k : kKinds) if (cfg.kind_enabled(k.name)) total_w += k.weight;
  if (!total_w) die(kExitEnv, "--only names no kind");
  int64_t deadline = g_start_ms + (int64_t)(cfg.hours * 3600.0 * 1000.0);
  int64_t last_print = g_start_ms;
  const char* state = "done";

  while (!g_stop) {
    if (mono_ms() >= deadline) break;
    if (!cfg.stop_file.empty() && fs::exists(cfg.stop_file)) { state = "stopped"; break; }
    if (g_findings_written >= cfg.max_findings) { state = "findings"; break; }

    uint64_t seed = mrng();
    Rng rng(seed);
    ++g_iteration;
    int w = (int)(rng() % (uint64_t)total_w);
    const Kind* kind = nullptr;
    for (auto& k : kKinds) {
      if (!cfg.kind_enabled(k.name)) continue;
      if (w < k.weight) { kind = &k; break; }
      w -= k.weight;
    }
    // larger histories get more attention, but not in proportion
    double tot = 0;
    for (auto& r : g_repos) tot += std::sqrt((double)r->commits.size());
    double x = std::uniform_real_distribution<double>(0, tot)(rng);
    RepoState* rs = g_repos.back().get();
    for (auto& r : g_repos) { double s = std::sqrt((double)r->commits.size()); if (x < s) { rs = r.get(); break; } x -= s; }

    if (++rs->iterations % cfg.recycle_every == 0) {
      std::string why;
      if (!prepare_worker(*rs, why)) die(kExitEnv, why);
    }

    Ops ops;
    if (!kind->pick(*rs, rng, ops)) { ++g_stats[kind->name].nopick; continue; }
    Inflight f{kind->name, rs->e.name, rs->e.pin, seed, g_iteration, ops};
    f.ops = ops;
    { std::lock_guard<std::mutex> l(g_mx); g_inflight = f; }
    journal_set(f);
    g_op_started = mono_ms();
    Outcome out = run_guarded(*kind, *rs, ops);
    g_op_started = 0;
    journal_clear();
    record(*kind, *rs, out, f);

    int64_t now = mono_ms();
    if (now - last_print > 60000) {
      last_print = now;
      print_stats((double)(now - g_start_ms) / 3.6e6);
      write_ledger("running");
    }
  }

  print_stats((double)(mono_ms() - g_start_ms) / 3.6e6);
  for (auto& [k, n] : g_cov) std::cout << "[soak] coverage " << k << ": " << n << "\n";
  write_ledger(g_findings_written ? "findings" : state);
  int code = g_findings_written ? kExitFindings : kExitOk;
  if (cfg.inject_every) {
    if (g_findings_written) {
      write_file(cfg.ledger / ("inject-proof-" + cfg.aios_commit + ".txt"), "commit=" + cfg.aios_commit + "\nutc=" + utc() + "\nfindings=" + std::to_string(g_findings_written) + "\n");
      std::cout << "[soak] fault injection was caught " << g_findings_written << " time(s): the gate bites\n";
    } else {
      std::cout << "[soak] fault injection produced NO finding: the gate would not catch a discrepancy\n";
      code = kExitHarness;
    }
  }
  std::cout << (code == kExitOk ? "[soak] clean\n" : "[soak] DISCREPANCIES FOUND - see " + cfg.findings.string() + "\n") << std::flush;
  return code;
}
