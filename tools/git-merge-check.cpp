// The kernel's diff and merge, held against real git.
//
//   aios-git-merge-check [cases]        (default 1500; AIOS_GIT_FUZZ_CASES overrides)
//
// Real git is the only opinion that counts about what a merge should produce, so
// most of this is differential: random inputs go through git_merge3 and through
// `git merge-file -p`, through git_unified_hunks and `git diff --no-index`,
// through git_merge_trees and `git merge-tree --write-tree`, and through
// git_merge_bases and `git merge-base --all`, and the bytes must be identical.
//
// The random texts are built so the diff is unambiguous: every text is a
// subsequence of one master sequence of distinct lines, so any two of them have
// exactly one longest common subsequence and any correct diff equals git's. A
// second generator uses a tiny alphabet, where diffs are ambiguous; there the
// merge is held to its invariants instead, and agreement with git is reported.
//
// Every check counts; the exit code is nonzero if any failed.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "aios/hal/process.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/storage/object_store.hpp"
#include "aios/userland/git.hpp"
#include "aios/userland/git_merge.hpp"

using namespace aios;
using namespace aios::userland;
namespace fs = std::filesystem;

namespace {

int passed = 0, failed = 0;
void check(bool ok, const std::string& name, const std::string& detail = "") {
  if (ok) { ++passed; return; }
  ++failed;
  std::cout << "[FAIL] " << name << (detail.empty() ? "" : ": " + detail) << "\n";
}
void section(const std::string& name, int before_failed) {
  std::cout << (failed == before_failed ? "[PASS] " : "[FAIL] ") << name << "\n";
}

struct TempDir {
  std::string path;
  TempDir() {
    // Forward slashes: the path goes into sh command lines, and Git for Windows' sh takes C:/...
    static std::random_device rd;
    for (int tries = 0; tries < 100 && path.empty(); ++tries) {
      fs::path p = fs::temp_directory_path() / ("aios-git-merge-" + std::to_string(rd()));
      std::error_code ec;
      if (fs::create_directory(p, ec)) path = p.generic_string();
    }
    if (path.empty()) { std::cerr << "cannot make a temporary directory\n"; std::exit(1); }
  }
  ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

std::string g_home;   // an empty HOME, so the user's git config cannot change git's answers

struct Run { int status = -1; std::string out; };

// A POSIX sh: /bin/sh, or on Windows the one beside git in a Git for Windows install.
std::string find_sh() {
#ifdef _WIN32
  std::string git = hal::find_program({"git"});
  if (git.empty()) return {};
  for (fs::path dir = fs::path(git).parent_path(); !dir.empty() && dir != dir.root_path(); dir = dir.parent_path())
    for (auto rel : {"usr/bin/sh.exe", "bin/sh.exe"})
      if (fs::exists(dir / rel)) return (dir / rel).string();
  return hal::find_program({"sh"});
#else
  return "/bin/sh";
#endif
}
Run sh(const std::string& cmd) {
  std::string full = "HOME=" + g_home + " GIT_CONFIG_NOSYSTEM=1 GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t "
                     "GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@t GIT_AUTHOR_DATE='1700000000 +0000' "
                     "GIT_COMMITTER_DATE='1700000000 +0000' " + cmd;
  // Through sh, by way of the HAL's process layer rather than popen: the same on Linux, macOS
  // and Windows (where the sh is the one Git for Windows ships). stdout only, byte for byte -
  // hal::run_tool would merge in stderr and, on Windows, drop every '\r'.
  Run r;
  static const std::string shell = find_sh();
  if (shell.empty()) return r;
  static int counter = 0;
  static const std::string stem = (fs::temp_directory_path() / ("aios-sh-" + std::to_string(std::random_device{}()))).generic_string();
  std::string out = stem + "-" + std::to_string(counter++);
  hal::ProcessSpawn spec;
  spec.argv = {shell, "-c", "{ " + full + "\n} >'" + out + "'"};
  auto h = hal::spawn_process(spec);
  if (!h.ok()) return r;
  auto st = hal::wait_process(*h, 600000);
  (void)hal::release_process(*h);
  if (!st.ok()) return r;
  r.status = st->exit_code;
  std::ifstream f(out, std::ios::binary);
  std::ostringstream o;
  o << f.rdbuf();
  r.out = o.str();
  f.close();
  std::error_code ec;
  fs::remove(out, ec);
  return r;
}
void put(const std::string& path, const std::string& data) {
  fs::create_directories(fs::path(path).parent_path());
  std::ofstream(path, std::ios::binary) << data;
}
std::string show(const std::string& s) {
  std::string o;
  for (char c : s) { if (c == '\n') o += "\\n"; else o += c; }
  return o.size() > 400 ? o.substr(0, 400) + "..." : o;
}

std::mt19937_64 rng(20261002);
size_t pick(size_t n) { return (size_t)(rng() % n); }
bool chance(double p) { return std::uniform_real_distribution<double>(0, 1)(rng) < p; }

// Three texts as subsequences of one master sequence of distinct lines.
struct Triple { std::string base, ours, theirs; };
Triple unique_triple(const std::string& tag) {
  size_t m = 1 + pick(40);
  std::vector<char> in_base(m);
  for (auto& b : in_base) b = chance(0.75);
  double keep = 0.6 + 0.35 * std::uniform_real_distribution<double>(0, 1)(rng);
  auto side = [&]() {
    std::string s;
    for (size_t i = 0; i < m; ++i)
      if (in_base[i] ? chance(keep) : chance(0.15)) s += tag + std::to_string(i) + "\n";
    return s;
  };
  Triple t;
  for (size_t i = 0; i < m; ++i) if (in_base[i]) t.base += tag + std::to_string(i) + "\n";
  t.ours = side();
  t.theirs = side();
  if (chance(0.1)) t.theirs = t.ours;
  if (chance(0.1)) t.ours = t.base;
  return t;
}
// Ambiguous texts: few distinct lines, plenty of repeats.
std::string tiny_text() {
  static const char* words[] = {"a\n", "b\n", "c\n", "}\n", "\n"};
  std::string s;
  size_t n = pick(12);
  for (size_t i = 0; i < n; ++i) s += words[pick(5)];
  return s;
}
std::string mutate(const std::string& s) {
  auto lines = git_split_lines(s);
  std::vector<std::string> v(lines.begin(), lines.end());
  size_t edits = 1 + pick(3);
  static const char* words[] = {"a\n", "b\n", "c\n", "}\n", "\n", "x\n"};
  for (size_t e = 0; e < edits; ++e) {
    size_t at = v.empty() ? 0 : pick(v.size() + 1);
    int op = (int)pick(3);
    if (op == 0 || v.empty()) v.insert(v.begin() + (long)std::min(at, v.size()), words[pick(6)]);
    else if (op == 1 && at < v.size()) v.erase(v.begin() + (long)at);
    else if (at < v.size()) v[at] = words[pick(6)];
  }
  std::string o;
  for (auto& l : v) o += l;
  return o;
}

Run git_merge_file(const std::string& dir, const Triple& t) {
  put(dir + "/b", t.base);
  put(dir + "/o", t.ours);
  put(dir + "/t", t.theirs);
  return sh("git merge-file -p -L ours -L base -L theirs " + dir + "/o " + dir + "/b " + dir + "/t");
}

// ---------------------------------------------------------------------------------

void golden() {
  int before = failed;
  auto m = [](const char* b, const char* o, const char* t) { return git_merge3(b, o, t); };
  {
    auto r = m("1\n2\n3\n4\n5\n6\n7\n8\n9\n", "1\nTWO\n3\n4\n5\n6\n7\n8\n9\n", "1\n2\n3\n4\n5\n6\n7\nEIGHT\n9\n");
    check(r.clean && r.merged == "1\nTWO\n3\n4\n5\n6\n7\nEIGHT\n9\n", "disjoint edits merge", show(r.merged));
  }
  {
    auto r = m("1\n2\n3\n", "1\nX\n3\n", "1\nY\n3\n");
    check(!r.clean && r.merged == "1\n<<<<<<< ours\nX\n=======\nY\n>>>>>>> theirs\n3\n" && r.conflicts.size() == 1,
          "same line edited differently conflicts", show(r.merged));
  }
  {
    auto r = m("1\n2\n3\n", "1\nX\n3\n", "1\nX\n3\n");
    check(r.clean && r.merged == "1\nX\n3\n", "identical edits are taken once");
  }
  {
    auto r = m("1\n2\n3\n4\n", "1\nX\n3\n4\n", "1\n2\nY\n4\n");
    check(!r.clean, "adjacent edits conflict, as in git", show(r.merged));
  }
  {
    auto r = m("a\n", "a\nb", "a\nc");
    check(!r.clean && r.merged == "a\n<<<<<<< ours\nb\n=======\nc\n>>>>>>> theirs\n", "no newline at end inside a conflict", show(r.merged));
  }
  {
    std::string bin("a\0b", 3);
    auto r = git_merge3(bin, std::string("a\0c", 3), std::string("a\0d", 3));
    check(!r.clean && r.binary, "binary on both sides is not merged");
  }
  {
    std::string bin("a\0b", 3), o("a\0c", 3), t("a\0d", 3);
    GitMerge3Options mo;
    mo.favor = GitMergeFavor::Theirs;
    auto r = git_merge3(bin, o, t, mo);
    check(r.clean && r.binary && r.merged == t, "binary with favor theirs takes theirs, as git -X theirs does");
    mo.favor = GitMergeFavor::Union;
    r = git_merge3(bin, o, t, mo);
    check(r.clean && r.merged == o, "binary with favor union takes ours, as git's binary merge does");
  }
  check(git_unified_hunks("a\n", "a\n").empty(), "no hunks for equal texts");
  check(git_unified_hunks("", "x\n") == "@@ -0,0 +1 @@\n+x\n", "new file hunk", show(git_unified_hunks("", "x\n")));
  section("golden merge and diff cases", before);
}

void fuzz_merge3_vs_git(const std::string& dir, int cases) {
  int before = failed, shown = 0;
  for (int c = 0; c < cases; ++c) {
    Triple t = unique_triple(c % 3 ? "L" : " line ");
    auto ours = git_merge3(t.base, t.ours, t.theirs);
    auto real = git_merge_file(dir, t);
    int git_conflicts = real.status;
    bool ok = real.out == ours.merged && git_conflicts == (int)std::min<size_t>(ours.conflicts.size(), 127);
    if (!ok && shown++ < 3)
      check(false, "merge3 == git merge-file (case " + std::to_string(c) + ")",
            "\n  base  " + show(t.base) + "\n  ours  " + show(t.ours) + "\n  theirs" + show(t.theirs) +
                "\n  git   (" + std::to_string(git_conflicts) + ") " + show(real.out) + "\n  aios  (" +
                std::to_string(ours.conflicts.size()) + ") " + show(ours.merged));
    else if (!ok) ++failed;
    else ++passed;
  }
  section("git_merge3 matches `git merge-file -p` byte for byte on " + std::to_string(cases) + " cases", before);
}

void fuzz_favor_vs_git(const std::string& dir, int cases) {
  int before = failed, shown = 0;
  static const std::pair<GitMergeFavor, const char*> favors[] = {
      {GitMergeFavor::Ours, "--ours"}, {GitMergeFavor::Theirs, "--theirs"}, {GitMergeFavor::Union, "--union"}};
  for (int c = 0; c < cases; ++c) {
    Triple t = unique_triple("L");
    auto& [favor, flag] = favors[c % 3];
    GitMerge3Options mo;
    mo.favor = favor;
    auto ours = git_merge3(t.base, t.ours, t.theirs, mo);
    put(dir + "/b", t.base);
    put(dir + "/o", t.ours);
    put(dir + "/t", t.theirs);
    auto real = sh(std::string("git merge-file -p ") + flag + " -L ours -L base -L theirs " + dir + "/o " + dir + "/b " + dir + "/t");
    bool ok = real.out == ours.merged && real.status == 0 && ours.clean;
    if (!ok && shown++ < 3)
      check(false, std::string("merge3 ") + flag + " == git merge-file " + flag, "\n  git  " + show(real.out) + "\n  aios " + show(ours.merged));
    else if (!ok) ++failed;
    else ++passed;
  }
  section("git_merge3 --ours/--theirs/--union matches `git merge-file` on " + std::to_string(cases) + " cases", before);
}

void fuzz_diff_vs_git(const std::string& dir, int cases) {
  int before = failed, shown = 0;
  for (int c = 0; c < cases; ++c) {
    Triple t = unique_triple(c % 2 ? "fn_" : "  x");
    if (chance(0.15) && !t.ours.empty()) t.ours.pop_back();     // no newline at end
    if (chance(0.15) && !t.base.empty()) t.base.pop_back();
    put(dir + "/a", t.base);
    put(dir + "/b", t.ours);
    auto real = sh("git diff --no-index --no-color -U3 " + dir + "/a " + dir + "/b");
    size_t at = real.out.find("@@");
    std::string git_hunks = at == std::string::npos ? "" : real.out.substr(at);
    std::string ours = git_unified_hunks(t.base, t.ours);
    bool ok = git_hunks == ours;
    if (!ok && shown++ < 3)
      check(false, "unified hunks == git diff (case " + std::to_string(c) + ")",
            "\n  git  " + show(git_hunks) + "\n  aios " + show(ours));
    else if (!ok) ++failed;
    else ++passed;
  }
  section("git_unified_hunks matches `git diff --no-index` on " + std::to_string(cases) + " cases", before);
}

void merge_invariants(const std::string& dir, int cases) {
  int before = failed, agree = 0;
  for (int c = 0; c < cases; ++c) {
    std::string b = tiny_text(), o = mutate(b), t = mutate(b);
    auto ot = git_merge3(b, o, t);
    auto to = git_merge3(b, t, o);
    check(ot.clean == to.clean, "merge is symmetric in clean/conflict", show(b) + " | " + show(o) + " | " + show(t));
    if (ot.clean && to.clean) check(ot.merged == to.merged, "a clean merge is symmetric", show(ot.merged) + " vs " + show(to.merged));
    check(git_merge3(b, o, b).merged == o && git_merge3(b, b, t).merged == t && git_merge3(b, o, o).merged == o,
          "an unchanged side takes the other");
    // The diff is a correct edit script: applying it to a gives b.
    auto la = git_split_lines(b), lb = git_split_lines(o);
    std::string rebuilt;
    size_t i = 0;
    for (auto& e : git_diff_lines(la, lb)) {
      for (; i < e.a_start; ++i) rebuilt += la[i];
      for (size_t k = 0; k < e.b_len; ++k) rebuilt += lb[e.b_start + k];
      i = e.a_start + e.a_len;
    }
    for (; i < la.size(); ++i) rebuilt += la[i];
    check(rebuilt == o, "applying the diff rebuilds the target");
    auto real = git_merge_file(dir, Triple{b, o, t});
    if ((real.status == 0) == ot.clean && (!ot.clean || real.out == ot.merged)) ++agree;
  }
  section("merge invariants on " + std::to_string(cases) + " ambiguous cases", before);
  std::cout << "       (agreement with git where diffs are ambiguous: " << agree << "/" << cases
            << "; differences there are a different but valid alignment, not a defect)\n";
}

// -- trees and history, through real repositories ----------------------------------

struct RepoGen {
  std::string dir;
  uint64_t clock = 1700000000;
  std::string env() { ++clock; return "GIT_AUTHOR_DATE='" + std::to_string(clock) + " +0000' GIT_COMMITTER_DATE='" + std::to_string(clock) + " +0000' "; }
  void git(const std::string& args) { sh("git -C " + dir + " " + args + " >/dev/null 2>&1"); }
  std::string out(const std::string& args) {
    auto r = sh(env() + "git -C " + dir + " " + args);
    while (!r.out.empty() && r.out.back() == '\n') r.out.pop_back();
    return r.out;
  }
  // A tree of the given files (path -> content; mode 0755 when the name ends in .sh).
  std::string tree_of(const std::map<std::string, std::string>& files) {
    git("read-tree --empty");
    for (auto& [p, c] : files) {
      std::string tmp = dir + "/.blob";
      put(tmp, c);
      std::string oid = out("hash-object -w " + tmp);
      std::string mode = p.size() > 3 && p.substr(p.size() - 3) == ".sh" ? "100755" : "100644";
      git("update-index --add --cacheinfo " + mode + "," + oid + "," + p);
    }
    return out("write-tree");
  }
  std::string commit_tree(const std::string& tree, const std::vector<std::string>& parents, const std::string& msg) {
    std::string ps;
    for (auto& p : parents) ps += " -p " + p;
    return out("commit-tree " + tree + ps + " -m " + msg);
  }
  std::string commit_files(const std::map<std::string, std::string>& files, const std::string& parent, const std::string& msg) {
    return commit_tree(tree_of(files), parent.empty() ? std::vector<std::string>{} : std::vector<std::string>{parent}, msg);
  }
};

// A side of history: each file kept, edited, deleted, or renamed (and maybe
// edited too); sometimes a new file, sometimes a script, sometimes binary bytes.
std::map<std::string, std::string> evolve(const std::map<std::string, std::string>& base, int salt, bool renames = true) {
  std::map<std::string, std::string> s;
  auto edit = [&](const std::string& p, const std::string& c) {
    if (c.find('\0') != std::string::npos) return c + std::string("\0", 1) + std::to_string(salt);   // binary: changed bytes
    std::string out;
    for (auto l : git_split_lines(c)) if (!chance(0.12)) out += std::string(l);
    if (chance(0.6)) out += p.substr(0, p.find('.')) + ":new" + std::to_string(salt) + "-" + std::to_string(pick(1000)) + "\n";
    return out;
  };
  for (auto& [p, c] : base) {
    double r = std::uniform_real_distribution<double>(0, 1)(rng);
    if (r < 0.08) continue;                                         // delete
    if (renames && r < 0.25) {                                      // rename, edited half the time
      std::string to = (chance(0.5) ? "moved/" : "") + std::string("r") + std::to_string(salt) + "_" + p.substr(p.rfind('/') == std::string::npos ? 0 : p.rfind('/') + 1);
      if (s.count(to) || base.count(to)) to = "x" + to;
      s[to] = chance(0.5) ? c : edit(p, c);
      continue;
    }
    if (r < 0.55) { s[p] = c; continue; }                           // keep
    s[p] = edit(p, c);                                              // edit
  }
  if (chance(0.3)) s["added/" + std::to_string(salt) + "_" + std::to_string(pick(5)) + ".txt"] = "fresh " + std::to_string(rng()) + "\n";
  if (chance(0.1)) s["tool" + std::to_string(pick(2)) + ".sh"] = "echo " + std::to_string(salt) + "\n";
  return s;
}

std::map<std::string, std::string> base_files(bool binary) {
  std::map<std::string, std::string> base;
  size_t nfiles = 1 + pick(6);
  for (size_t f = 0; f < nfiles; ++f) {
    std::string p = (f % 2 ? "src/" : "") + std::string("f") + std::to_string(f) + (binary && f == 0 ? ".bin" : ".txt");
    std::string tag = p.substr(0, p.find('.')) + ":";
    std::string content;
    size_t n = 4 + pick(20);
    for (size_t i = 0; i < n; ++i) content += tag + std::to_string(i) + " some words to make lines long enough\n";
    if (binary && f == 0) content = std::string("BIN\0", 4) + content;
    base[p] = content;
  }
  return base;
}

struct GitMergeAnswer { int status = -1; std::string tree; std::set<std::string> conflicts; };
GitMergeAnswer git_merge_tree(RepoGen& g, const std::string& o, const std::string& t, const std::string& extra = "") {
  auto real = sh("git -C " + g.dir + " -c merge.directoryRenames=false merge-tree --write-tree --name-only --no-messages " +
                 extra + " " + o + " " + t);
  GitMergeAnswer a;
  a.status = real.status;
  std::istringstream lines(real.out);
  std::getline(lines, a.tree);
  for (std::string line; std::getline(lines, line) && !line.empty();) a.conflicts.insert(line);
  return a;
}

bool same_answer(const GitMergeAnswer& want, const GitCommitMerge& m, std::string* why) {
  std::set<std::string> ours;
  for (auto& pc : m.tree.conflicts) ours.insert(pc.path);
  bool ok = want.status == 0 ? (m.tree.clean && m.tree.tree == want.tree) : (!m.tree.clean && ours == want.conflicts);
  if (!ok) {
    std::string gl, al;
    for (auto& p : want.conflicts) gl += p + " ";
    for (auto& pc : m.tree.conflicts) al += pc.path + "(" + pc.kind + ") ";
    *why = "git status " + std::to_string(want.status) + " tree " + want.tree + " conflicts [" + gl + "]; aios clean " +
           std::to_string(m.tree.clean) + " tree " + m.tree.tree + " conflicts [" + al + "]";
  }
  return ok;
}

void trees_vs_git(Kernel& kernel, int cases) {
  int before = failed, shown = 0;
  int renamed_cases = 0;
  TempDir td;
  RepoGen g{td.path + "/r"};
  fs::create_directories(g.dir);
  g.git("init -q -b main");
  Git repo(kernel);
  if (auto r = repo.open(g.dir); !r.ok()) { check(false, "open the generated repository", r.error().str()); return; }

  for (int c = 0; c < cases; ++c) {
    auto base = base_files(c % 4 == 0);
    std::string b = g.commit_files(base, "", "base");
    std::string o = g.commit_files(evolve(base, 1), b, "ours");
    std::string t = g.commit_files(evolve(base, 2), b, "theirs");
    static const std::pair<GitMergeFavor, const char*> favors[] = {
        {GitMergeFavor::None, ""}, {GitMergeFavor::Ours, "-X ours"}, {GitMergeFavor::Theirs, "-X theirs"}};
    for (auto& [favor, flag] : favors) {
      if (favor != GitMergeFavor::None && c % 3) continue;
      auto want = git_merge_tree(g, o, t, flag);
      GitTreeMergeOptions mo;
      mo.favor = favor;
      auto m = git_merge_commits(repo, o, t, "merge", true, mo);
      if (!m.ok()) { check(false, "git_merge_commits ran", m.error().str()); continue; }
      if (favor == GitMergeFavor::None && (!m->tree.renames_ours.empty() || !m->tree.renames_theirs.empty())) ++renamed_cases;
      std::string why;
      bool ok = same_answer(want, *m, &why);
      if (!ok && shown++ < 4) check(false, std::string("tree merge == git merge-tree ") + flag + " (case " + std::to_string(c) + ")", why);
      else if (!ok) ++failed;
      else ++passed;
      if (m->kind == GitCommitMerge::Kind::Merged) {
        std::string parents = g.out("rev-list --parents -n 1 " + m->commit);
        check(parents == m->commit + " " + o + " " + t, "the merge commit has ours then theirs as parents", parents);
      }
    }
  }
  section("git_merge_trees matches `git merge-tree --write-tree` (plain, -X ours, -X theirs) on " + std::to_string(cases) +
              " repositories, " + std::to_string(renamed_cases) + " of them with renames",
          before);
  int b2 = failed;
  auto fsck = sh("git -C " + g.dir + " fsck --no-progress --strict 2>&1");
  check(fsck.status == 0, "real git fsck --strict on every object aios wrote", fsck.out);
  section("git fsck --strict is clean after the merges", b2);
}

void renames_vs_git(Kernel& kernel, int cases) {
  int before = failed, shown = 0, found = 0;
  TempDir td;
  RepoGen g{td.path + "/r"};
  fs::create_directories(g.dir);
  g.git("init -q -b main");
  Git repo(kernel);
  if (auto r = repo.open(g.dir); !r.ok()) { check(false, "open the rename repository", r.error().str()); return; }
  for (int c = 0; c < cases; ++c) {
    auto base = base_files(false);
    std::string bt = g.tree_of(base);
    std::string st = g.tree_of(evolve(base, c));
    auto real = sh("git -C " + g.dir + " diff-tree -r -M --name-status " + bt + " " + st);
    std::set<std::string> want, got;
    std::istringstream ls(real.out);
    for (std::string l; std::getline(ls, l);)
      if (!l.empty() && l[0] == 'R') {
        std::istringstream f(l);
        std::string code, from, to;
        f >> code >> from >> to;
        want.insert(from + " -> " + to + " " + std::to_string(std::atoi(code.c_str() + 1)));
      }
    auto r = git_detect_renames(repo, bt, st);
    if (!r.ok()) { check(false, "git_detect_renames ran", r.error().str()); continue; }
    for (auto& x : *r) got.insert(x.from + " -> " + x.to + " " + std::to_string(x.percent));
    found += (int)got.size();
    bool ok = want == got;
    if (!ok && shown++ < 3) {
      std::string w, a;
      for (auto& x : want) w += x + "; ";
      for (auto& x : got) a += x + "; ";
      check(false, "renames == git diff-tree -M (case " + std::to_string(c) + ")", "git [" + w + "] aios [" + a + "]");
    } else if (!ok) ++failed;
    else ++passed;
  }
  section("git_detect_renames matches `git diff-tree -M` (pairs and R-percent) on " + std::to_string(cases) +
              " trees, " + std::to_string(found) + " renames",
          before);
}

// Criss-cross: two branches that each merged the other, then moved on.
void criss_cross_vs_git(Kernel& kernel, int cases) {
  int before = failed, shown = 0, multi = 0;
  TempDir td;
  RepoGen g{td.path + "/r"};
  fs::create_directories(g.dir);
  g.git("init -q -b main");
  Git repo(kernel);
  if (auto r = repo.open(g.dir); !r.ok()) { check(false, "open the criss-cross repository", r.error().str()); return; }
  auto merged_tree = [&](const std::string& x, const std::string& y, const std::string& fallback_tree) {
    auto a = git_merge_tree(g, x, y);
    return a.status == 0 ? a.tree : fallback_tree;
  };
  for (int c = 0; c < cases; ++c) {
    auto base = base_files(false);
    std::string b = g.commit_files(base, "", "base");
    auto fx = evolve(base, 1, false), fy = evolve(base, 2, false);
    std::string x = g.commit_files(fx, b, "x");
    std::string y = g.commit_files(fy, b, "y");
    std::string xt = g.out("rev-parse " + x + "^{tree}"), yt = g.out("rev-parse " + y + "^{tree}");
    std::string o1 = g.commit_tree(merged_tree(x, y, xt), {x, y}, "o1");
    std::string t1 = g.commit_tree(merged_tree(y, x, yt), {y, x}, "t1");
    std::map<std::string, std::string> fo, ft;
    for (auto& [p, s] : fx) fo[p] = s;
    for (auto& [p, s] : fy) ft[p] = s;
    std::string o = g.commit_tree(g.tree_of(evolve(fo, 3, false)), {o1}, "o");
    std::string t = g.commit_tree(g.tree_of(evolve(ft, 4, false)), {t1}, "t");
    auto bases = git_merge_bases(repo, o, t);
    if (bases.ok() && bases->size() > 1) ++multi;
    auto want = git_merge_tree(g, o, t);
    auto m = git_merge_commits(repo, o, t, "merge");
    if (!m.ok()) { check(false, "criss-cross merge ran", m.error().str()); continue; }
    std::string why;
    bool ok = same_answer(want, *m, &why);
    if (!ok && shown++ < 3) check(false, "criss-cross == git merge-tree (case " + std::to_string(c) + ")", why);
    else if (!ok) ++failed;
    else ++passed;
  }
  section("criss-cross merges match `git merge-tree` on " + std::to_string(cases) + " histories, " +
              std::to_string(multi) + " with more than one merge base",
          before);
}

// A merge driver decides a path the built-in merge cannot: here, a "lockfile"
// whose merge is the sorted union of both sides' lines.
void driver_check(Kernel& kernel) {
  int before = failed;
  TempDir td;
  RepoGen g{td.path + "/r"};
  fs::create_directories(g.dir);
  g.git("init -q -b main");
  Git repo(kernel);
  if (auto r = repo.open(g.dir); !r.ok()) { check(false, "open the driver repository", r.error().str()); return; }
  std::string b = g.commit_files({{"deps.lock", "a 1\nc 1\n"}, {"img.bin", std::string("P\0\1", 3)}}, "", "b");
  std::string o = g.commit_files({{"deps.lock", "a 1\nb 1\nc 1\n"}, {"img.bin", std::string("P\0\2", 3)}}, b, "o");
  std::string t = g.commit_files({{"deps.lock", "a 1\nc 1\nd 1\n"}, {"img.bin", std::string("P\0\3", 3)}}, b, "t");
  auto plain = git_merge_commits(repo, o, t, "m");
  check(plain.ok() && !plain->tree.clean, "without a driver the lockfile and the binary both conflict");
  GitTreeMergeOptions mo;
  mo.driver = [](const std::string& path, std::string_view, std::string_view ours, std::string_view theirs) -> std::optional<GitMerge3> {
    if (path == "img.bin") { GitMerge3 r; r.merged = std::string(theirs); return r; }   // e.g. "newest image wins"
    if (path.size() < 5 || path.substr(path.size() - 5) != ".lock") return std::nullopt;
    std::set<std::string> lines;
    for (auto l : git_split_lines(ours)) lines.insert(std::string(l));
    for (auto l : git_split_lines(theirs)) lines.insert(std::string(l));
    GitMerge3 r;
    for (auto& l : lines) r.merged += l;
    return r;
  };
  auto driven = git_merge_commits(repo, o, t, "m", true, mo);
  check(driven.ok() && driven->tree.clean, "with the driver the merge is clean");
  if (driven.ok() && driven->tree.clean) {
    check(g.out("cat-file -p " + driven->tree.tree + ":deps.lock") == "a 1\nb 1\nc 1\nd 1", "the driver's merged lockfile is in the tree");
    check(g.out("cat-file -p " + driven->tree.tree + ":img.bin | od -An -tx1 | tr -d ' '") == "500003", "the driver's binary choice is in the tree");
  }
  section("merge drivers resolve paths the line merge cannot", before);
}

void merge_bases_vs_git(Kernel& kernel, int cases) {
  int before = failed;
  TempDir td;
  RepoGen g{td.path + "/r"};
  fs::create_directories(g.dir);
  g.git("init -q -b main");
  Git repo(kernel);
  if (auto r = repo.open(g.dir); !r.ok()) { check(false, "open the DAG repository", r.error().str()); return; }
  std::string empty_tree = g.out("mktree </dev/null");
  for (int c = 0; c < cases; ++c) {
    std::vector<std::string> commits;
    size_t n = 2 + pick(14);
    for (size_t i = 0; i < n; ++i) {
      std::string parents;
      if (i > 0) {
        parents += " -p " + commits[pick(i)];
        if (i > 2 && chance(0.35)) {
          std::string second = commits[pick(i)];
          if (parents.find(second) == std::string::npos) parents += " -p " + second;
        }
      }
      commits.push_back(g.out("commit-tree " + empty_tree + parents + " -m c" + std::to_string(c) + "_" + std::to_string(i)));
    }
    std::string a = commits[pick(n)], b = commits[pick(n)];
    auto real = g.out("merge-base --all " + a + " " + b);
    std::vector<std::string> want;
    std::istringstream ls(real);
    for (std::string l; std::getline(ls, l);) if (!l.empty()) want.push_back(l);
    std::sort(want.begin(), want.end());
    auto got = git_merge_bases(repo, a, b);
    check(got.ok() && *got == want, "merge bases == git merge-base --all (case " + std::to_string(c) + ")",
          got.ok() ? std::to_string(got->size()) + " vs " + std::to_string(want.size()) : got.error().str());
  }
  section("git_merge_bases matches `git merge-base --all` on " + std::to_string(cases) + " random DAGs", before);
}

}  // namespace

int main(int argc, char** argv) {
  int cases = 1500;
  if (const char* e = std::getenv("AIOS_GIT_FUZZ_CASES")) cases = std::atoi(e);
  if (argc > 1) cases = std::atoi(argv[1]);
  if (sh("git --version >/dev/null 2>&1").status != 0) {
    std::cout << "this check needs real git to compare against\n";
    return 1;
  }
  TempDir home, work;
  g_home = home.path;

  golden();
  fuzz_merge3_vs_git(work.path, cases);
  fuzz_favor_vs_git(work.path, cases);
  fuzz_diff_vs_git(work.path, cases);
  merge_invariants(work.path, cases / 3);

  TempDir root;
  KernelConfig kcfg;
  kcfg.root = root.path;
  kcfg.residency = test_config;
  auto k = Kernel::boot(kcfg);
  if (!k.ok()) { check(false, "kernel boot", k.error().str()); }
  else {
    renames_vs_git(**k, std::max(50, cases / 10));
    trees_vs_git(**k, std::max(50, cases / 10));
    criss_cross_vs_git(**k, std::max(30, cases / 20));
    driver_check(**k);
    merge_bases_vs_git(**k, std::max(100, cases / 5));
  }

  std::cout << "\n-------------------------------------------------------------\n";
  std::cout << "  " << passed + failed << " TOTAL   " << passed << " PASSED   " << failed << " FAILED\n";
  return failed == 0 ? 0 : 1;
}
