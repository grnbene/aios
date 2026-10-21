#pragma once
// Diff and merge for the kernel's git: what turns two lines of history into one.
//
// Line diff is Myers' O(ND) algorithm in linear space. The three-way merge follows
// git's own rules (xdiff's xdl_merge at the level `git merge-file` uses):
// changes that overlap OR touch in the base conflict; identical changes on both
// sides are taken once; a conflict is refined by diffing the two sides, so lines
// they agree on drop out of it; and conflicts separated by three lines or fewer,
// or only by lines with no letters or digits, are joined. The same inputs give
// the same bytes `git merge-file -p` writes, conflict markers included, and
// tools/git-merge-check.cpp holds that against real git.
//
// The tree merge is git's (merge-ort's) per-path rule set: a path changed on one
// side only takes that side, both sides changed goes through the three-way
// merge, and delete/modify, add/add, mode and file/directory clashes are
// conflicts. Renames are found on each side the way git finds them (identical
// content first, then diffcore's similarity score at 50%), so a file one side
// renamed and the other edited merges at its new name. A criss-cross history
// (more than one merge base) is merged against a virtual base built by merging
// the bases, as merge-ort does. Binary content cannot be merged line by line by
// anyone, git included: both sides changing it is a conflict unless a favor
// (git's -X ours / -X theirs) or a merge driver for that path says otherwise.
// Directory renames are not inferred.
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "aios/core/result.hpp"
#include "aios/userland/git.hpp"

namespace aios::userland {

// Lines of a text, each keeping its '\n'; a last line without one is still a line.
std::vector<std::string_view> git_split_lines(std::string_view text);

// One change: a[a_start, a_start+a_len) becomes b[b_start, b_start+b_len).
struct GitDiffEdit { size_t a_start = 0, a_len = 0, b_start = 0, b_len = 0; };
std::vector<GitDiffEdit> git_diff_lines(const std::vector<std::string_view>& a,
                                        const std::vector<std::string_view>& b);

// The same edits with change groups placed by the indent heuristic, as `git diff` does by default.
// Lines are compared as `a` and `b` hold them; the heuristic reads `shown_a`/`shown_b` (the lines as written)
// when those differ, as with whitespace ignored in the comparison.
std::vector<GitDiffEdit> git_diff_lines_indent(const std::vector<std::string_view>& a,
                                               const std::vector<std::string_view>& b,
                                               const std::vector<std::string_view>* shown_a = nullptr,
                                               const std::vector<std::string_view>* shown_b = nullptr);

// The hunks `git diff` prints for a -> b (no "diff --git" / "---" / "+++" header):
// `context` lines around each change, "@@ -a,b +c,d @@ <function line>", and
// "\ No newline at end of file" where a side ends without one. Empty when equal.
std::string git_unified_hunks(std::string_view a, std::string_view b, int context = 3);

// Three-way merge of one file. `merged` is the file to write: on a conflict it
// holds git's markers ("<<<<<<< ours" / "=======" / ">>>>>>> theirs").
struct GitMergeConflict {
  size_t ours_line = 0;           // where the conflict starts in `ours` (0-based)
  std::string ours, theirs;       // what each side has there
};
struct GitMerge3 {
  bool clean = true;
  bool binary = false;            // a NUL byte on a side that both changed: not merged
  std::string merged;
  std::vector<GitMergeConflict> conflicts;
};
// What to do with a conflicting hunk instead of marking it up: git's
// --ours / --theirs / --union (merge-file) and -X ours / -X theirs (merge).
enum class GitMergeFavor : uint8_t { None, Ours, Theirs, Union };

// Which line diff a merge is built on. `git merge-file` uses Myers; git's tree merge
// (merge-ort) uses histogram, and on some files the two lead to different merges.
enum class GitDiffAlgorithm : uint8_t { Myers, Histogram, Minimal, Patience };

// What `git diff` is asked of the line diff: how many lines of context, which whitespace differences to ignore,
// which algorithm, and so on. Lines it hands back point into the two texts, which must outlive them.
struct GitDiffTextOptions {
  int context = 3;
  int inter_hunk_context = 0;               // --inter-hunk-context
  bool function_context = false;            // -W: whole functions around each change
  GitDiffAlgorithm algorithm = GitDiffAlgorithm::Myers;
  bool indent_heuristic = true;
  bool ignore_space_at_eol = false, ignore_space_change = false, ignore_all_space = false, ignore_cr_at_eol = false;
  bool ignore_blank_lines = false;
  std::function<bool(std::string_view)> ignore_line;   // -I: a change whose every line matches starts no hunk
};
struct GitDiffLine { char tag; std::string_view text; };   // ' ', '-' or '+'; the text keeps its '\n'
struct GitDiffHunk {
  size_t old_start = 0, old_count = 0, new_start = 0, new_count = 0;   // 0-based starts
  std::string func;                                                     // the "@@ ... @@ <func>" text
  std::vector<GitDiffLine> lines;
};
std::vector<GitDiffHunk> git_diff_hunks(std::string_view a, std::string_view b, const GitDiffTextOptions& o);
std::string git_unified_hunks(std::string_view a, std::string_view b, const GitDiffTextOptions& o);

struct GitMerge3Options {
  std::string ours_label = "ours", theirs_label = "theirs";
  GitMergeFavor favor = GitMergeFavor::None;
  int marker_size = 7;            // "<<<<<<<"; merges of merge bases use more, so markers nest readably
  // Join conflicts separated only by lines with no letters or digits (what
  // `git merge-file` does). Off is what a tree merge does; the clean result is
  // the same either way, only how conflicts are grouped differs.
  bool join_non_alnum = true;
  GitDiffAlgorithm algorithm = GitDiffAlgorithm::Myers;   // git_merge_trees sets Histogram, as merge-ort does
};
GitMerge3 git_merge3(std::string_view base, std::string_view ours, std::string_view theirs,
                     const GitMerge3Options& opt = {});

// Best common ancestors of two commits: common ancestors that are not ancestors
// of another common ancestor. One in ordinary history; more after a criss-cross.
Result<std::vector<std::string>> git_merge_bases(const Git& repo, const std::string& a, const std::string& b);
// Whether `ancestor` is reachable from `commit` (a commit is its own ancestor).
Result<bool> git_is_ancestor(const Git& repo, const std::string& ancestor, const std::string& commit);

struct GitPathConflict {
  std::string path;
  // content | binary | modify/delete | add/add | mode | file/directory | type |
  // rename/delete | rename/rename | rename/add
  std::string kind;
  GitMerge3 file;                 // for content (and add/add) conflicts: the hunks and the marked-up text
};
struct GitRename { std::string from, to; int percent = 100; };

// A merge driver: given one path's three versions (base empty when the path is
// new on both sides), return the merged file, or nullopt to let the built-in
// merge handle it. This is how a lockfile, a JSON document or a binary format
// gets merged by something that understands it.
using GitMergeDriver = std::function<std::optional<GitMerge3>(
    const std::string& path, std::string_view base, std::string_view ours, std::string_view theirs)>;

struct GitTreeMergeOptions {
  GitMergeFavor favor = GitMergeFavor::None;   // applied to content and binary conflicts, as git does
  bool detect_renames = true;
  int rename_threshold = 50;                   // percent similarity, git's default
  GitMergeDriver driver;
  std::string ours_label = "ours", theirs_label = "theirs";
  int call_depth = 0;                          // > 0 while merging merge bases into a virtual one
  // When the line merge of a file both sides changed conflicts, try a merge of its syntax tree (functions,
  // classes, imports... as units: git_structural_merge) before declaring the conflict. Off by default: git does
  // not do this, and `git merge` here matches git unless merge.structural is set.
  bool structural = false;
};

struct GitTreeMerge {
  bool clean = true;
  std::vector<GitRename> renames_ours, renames_theirs;   // what each side renamed, relative to the base
  std::vector<GitIndexEntry> entries;          // the merged tree as flat paths (conflicted paths keep `ours`)
  std::vector<std::string> auto_merged;        // paths both sides changed that merged cleanly
  std::vector<GitPathConflict> conflicts;
  std::string tree;                            // written only when clean
};
// Merge `theirs_tree` into `ours_tree` over `base_tree` ("" = the empty tree).
// Merged blobs and, when clean, the result tree are written to the repository.
Result<GitTreeMerge> git_merge_trees(Git& repo, const std::string& base_tree,
                                     const std::string& ours_tree, const std::string& theirs_tree,
                                     const GitTreeMergeOptions& opt = {});

// Renames from `before` to `after` (two trees), found as git's diffcore-rename
// finds them: exact matches, then similarity >= threshold, best scores first.
Result<std::vector<GitRename>> git_detect_renames(const Git& repo, const std::string& before_tree,
                                                  const std::string& after_tree, int threshold_percent = 50);
// diffcore's similarity of two blobs, 0..100 (what `git diff -M` prints as R<nn>).
int git_similarity(std::string_view a, std::string_view b);
// The same from each side's span counts, computed once per blob (a rename search compares every pair).
struct GitSpanCounts { std::vector<std::pair<uint32_t, uint32_t>> spans; size_t size = 0; };   // (hash, bytes) by hash
GitSpanCounts git_span_counts(std::string_view text);
int git_similarity(const GitSpanCounts& a, const GitSpanCounts& b);
int git_similarity_score(const GitSpanCounts& a, const GitSpanCounts& b);   // on git's scale, 0..60000

// The whole thing for two commits: merge base, tree merge, and when clean a
// two-parent commit (ours first). With more than one merge base the bases are
// merged into a virtual one first (recursively), as merge-ort does.
struct GitCommitMerge {
  enum class Kind { UpToDate, FastForward, Merged, Conflicted } kind = Kind::Conflicted;
  std::string base, commit;                    // commit: the result (theirs for a fast-forward)
  GitTreeMerge tree;
};
Result<GitCommitMerge> git_merge_commits(Git& repo, const std::string& ours, const std::string& theirs,
                                         const std::string& message, bool allow_fast_forward = true,
                                         const GitTreeMergeOptions& opt = {});

}  // namespace aios::userland
