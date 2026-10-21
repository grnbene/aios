#pragma once
// The change layer: history as changes rather than commits, on git's own object format.
//
// A change is a commit with a stable identity: a `change-id` header that survives every rewrite of it (a new
// description, new content, a new parent). Commits git or anyone else wrote, without the header, get an id derived
// from their commit id, so every commit is a change.
//
// In a repository with the change layer on (`git change init`, config change.enabled):
//   - The working tree IS a commit, the working-copy change "@" (refs/changes/wc/<workspace>, HEAD detached at it).
//     Every command that looks at the repository snapshots the files into it first: there is no staging area and
//     nothing to stash. The index is kept equal to @'s tree and serves only as the stat cache.
//   - Rewriting a change rebases everything built on it (its descendants), and moves the branches that pointed at
//     the old version along with it.
//   - A merge or rebase always completes. A path both sides changed incompatibly is written with conflict markers,
//     and the commit records the sides in a `conflict-sides <tree>` header; resolving it is an ordinary later edit.
//   - Changes stay visible through refs/changes/heads/<change-id> (one per change nothing else keeps visible);
//     what a rewrite or abandon hides is still in the object store, and the operation log can bring it back.
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "cli.hpp"

namespace aios_git::change {

// --- identity ---------------------------------------------------------------------------------------------------
bool enabled(Ctx& c);                                 // change.enabled in the repository's configuration
std::string new_change_id(Ctx& c);                    // 32 letters k..z, as jj writes them
std::string derived_change_id(const std::string& commit_oid);
std::string change_id_of(const GitCommit& cm);         // the header, else derived
std::string short_id(const std::string& change_id);    // the first 12 letters

// --- writing commits --------------------------------------------------------------------------------------------
struct CommitSpec {
  std::string tree;
  std::vector<std::string> parents;
  std::string message;           // as stored (may be empty: "no description set")
  std::string author;            // "Name <email> <seconds> <tz>"; empty: the current identity
  std::string committer;         // empty: the current identity, now
  std::string change_id;         // empty: a new one
  std::string conflict_sides;    // a tree (see Conflicts) or ""
  std::vector<std::pair<std::string, std::string>> other_headers;   // kept through rewrites (encoding...)
};
CommitSpec spec_of(Ctx& c, const GitCommit& cm);
std::string write_commit(Ctx& c, CommitSpec spec);

// --- conflicts --------------------------------------------------------------------------------------------------
// One conflicted path as recorded in a commit: the three sides (an absent side has present=false), the labels the
// markers carry, the conflict's kind (content, modify/delete, add/add, binary, ...) and the blob the commit's tree
// held for the path when the conflict was recorded ("materialized").
struct Side { std::string oid; uint32_t mode = 0100644; bool present = false; };
struct PathConflict {
  Side base, ours, theirs;
  std::string kind = "content";
  std::string ours_label, theirs_label;
  std::string materialized;
};
using Conflicts = std::map<std::string, PathConflict>;
Conflicts read_conflicts(Ctx& c, const GitCommit& cm);      // from the commit's conflict-sides header
std::string write_conflicts(Ctx& c, const Conflicts& cs);     // "" when empty
bool has_markers(const std::string& text);
// Which of `candidates` are still conflicts in `tree`: the path's file still holds conflict markers, or still is
// exactly the blob that was written when the conflict was recorded.
Conflicts still_conflicted(Ctx& c, const std::string& tree, const Conflicts& candidates);

// A three-way tree merge that always completes: conflicted paths are written with markers (or, where there are no
// lines to mark up, with one side's content) and come back in `conflicts`.
struct MergedTree { std::string tree; Conflicts conflicts; };
MergedTree merge_trees(Ctx& c, const std::string& base_tree, const std::string& ours_tree, const std::string& theirs_tree,
                       const std::string& ours_label, const std::string& theirs_label);
// The tree of a commit with these parents and no changes of its own: one parent's tree, or the parents merged.
MergedTree parents_tree(Ctx& c, const std::vector<std::string>& parents);

// --- the repository's changes ----------------------------------------------------------------------------------
// GIT_CHANGE_WORKSPACE when set; else a linked work tree's own name (git worktree add); else "default".
std::string workspace(Ctx& c);
std::string wc_ref(Ctx& c);                             // refs/changes/wc/<workspace>
std::string wc_commit(Ctx& c);                          // "" when there is none yet
std::map<std::string, std::string> head_refs(Ctx& c);   // change id -> commit (refs/changes/heads/*)
// Commits nothing may rewrite (without --ignore-immutable): everything remote-tracking refs and tags reach, and
// whatever change.immutable names.
bool is_immutable(Ctx& c, const std::string& oid);
std::vector<std::string> immutable_heads(Ctx& c);
// Every commit a change command shows and may rewrite: what the heads, the working copies and the local branches
// reach, down to (and including) the first immutable commits. `limit` caps the walk (0: no cap).
std::vector<std::string> visible_commits(Ctx& c, size_t limit = 0);
// A revision as the change commands read it: @ (the working copy), @- / @-- (its parents), a change id or a unique
// prefix of one, or anything git itself resolves (commit ids, branches, HEAD~2, ...).
std::string resolve_rev(Ctx& c, const std::string& spec);
std::vector<std::string> resolve_revs(Ctx& c, const std::string& spec);   // @- can name several (a merge)
// Commits holding this change id among the visible ones (more than one: the change is divergent).
std::vector<std::string> commits_of_change(Ctx& c, const std::string& change_id);

// --- rewriting ---------------------------------------------------------------------------------------------------
// Every rewrite goes through a transaction. finish() rebases the descendants of everything rewritten or abandoned
// (each keeps its change id; a conflict the rebase meets is recorded in the commit, never stops it), moves the
// branches and head refs that pointed at old versions, checks out the working-copy commit if it changed, and
// records the operation in the operation log.
struct WcMoved {};

class Tx {
 public:
  Tx(Ctx& c, std::string description);
  void rewrite(const std::string& old_oid, const std::string& new_oid);
  void abandon(const std::string& oid);             // its children move onto its parents
  // `old_oid` leaves its place: its children move onto its parents, while the refs that named it (branches, its
  // head ref, a working copy) follow it to `new_oid` (git change rebase -r).
  void move_out(const std::string& old_oid, const std::string& new_oid);
  void add_head(const std::string& oid);            // keep a new commit visible
  void set_wc(const std::string& oid);              // make it the working-copy commit
  // A commit's new version after this transaction (itself when untouched; its parents' for an abandoned one is
  // handled by new_parents()).
  std::string mapped(const std::string& oid) const;
  std::vector<std::string> new_parents(const std::vector<std::string>& parents) const;
  // Rebase descendants, update refs, check out @, record the operation. Returns how many descendants it rebased.
  size_t finish();
  bool allow_immutable = false;
  // @ is swapped from what it was when this transaction began; another process having moved it meanwhile (the
  // fsmonitor daemon's snapshot takes no lock) throws WcMoved when this is set (the snapshot starts again), and is
  // fatal otherwise. Nothing has been written then.
  bool retry_if_wc_moved = false;
  std::string kind = "normal";                   // the operation's kind (snapshot: bookkeeping undo passes over)
  const std::map<std::string, std::string>& rewrites() const { return rewritten_; }

 private:
  Ctx& c_;
  std::string description_;
  std::map<std::string, std::string> rewritten_;                 // old -> new
  std::map<std::string, std::vector<std::string>> abandoned_;    // old -> its parents
  std::map<std::string, std::string> moved_;                     // old -> where its refs go
  std::map<std::string, std::vector<std::string>> moved_parents_;
  std::vector<std::string> new_heads_;
  std::string new_wc_;
  std::string wc_before_;
};

// Rebase one commit onto new parents: its own changes (its tree against its old parents') applied to the new
// parents' tree. Returns the new commit's id (the old one when nothing changes).
std::string rebase_commit(Ctx& c, const GitCommit& cm, const std::vector<std::string>& new_parents);

// --- the working copy --------------------------------------------------------------------------------------------
// Bring the files on disk into @: a new version of @ when they differ from it (descendants rebased, an operation
// recorded). Also notices HEAD moved by a plain git command and starts a new @ on top of where it went. Returns
// whether anything was recorded.
bool snapshot(Ctx& c);
// Whether this workspace has an @, starting one on top of HEAD for a workspace other than "default" that has none
// yet (a new work tree's first command).
bool ensure_wc(Ctx& c);
// The fsmonitor daemon's snapshot, without the repository lock: @ built from the files as objects, then HEAD, @ and its
// change head swapped together from what was read (Git::compare_and_swap_refs), the operation appended the same way.
// A command running, or one that moved @ first, wins: its own snapshot takes the files. False when this needs the
// locked path (a git command moved HEAD, or @ has descendants to rebase).
bool snapshot_unlocked(Ctx& c);
// Make `entries` (a whole index) the working-copy commit's tree: the index is written, @ rewritten when its tree
// changes, and descendants rebased.
void set_wc_index(Ctx& c, const std::vector<GitIndexEntry>& entries, const std::string& description, bool retry_if_wc_moved = false);
// Plain git commands that mean something else here (commit, stash): handled, or refused, returning the exit code;
// -1 when the command is not one of them.
int intercept(Ctx& c);
// git status (the long form) with the layer on: the workspace, what its @ changed from @-, @ and @-. A snapshot is
// taken first, so the answer is the files as they are now; a plain-git status compared them with @ and was always
// empty. -1 when this workspace has no @ (plain status applies).
int status_of_workspace(Ctx& c);
// Put `tree` on disk and in the index, from what the index holds now (which snapshot() made equal to the disk).
void check_out_tree(Ctx& c, const std::string& from_tree, const std::string& to_tree);
// Drop what this invocation has cached about refs and visibility (after something else moved refs).
void forget(Ctx& c);
// After refs were put back one by one (an undo), a commit may still sit on an older version of a change than the one
// the refs now hold: it is rebased onto that version.
void evolve(Ctx& c, const std::string& description);
// The change commands begin with this: the repository open, the change layer on, the working copy snapshotted.
void open_changes(Ctx& c);
// Turn the change layer on: @ is made on top of HEAD from whatever the work tree and the index hold.
void enable(Ctx& c);
// Turn it off: HEAD goes back onto the branch at @'s parent (or `branch`, moved forward to it when behind), the index
// to that commit, and what @ held stays in the work tree as uncommitted changes.
void disable(Ctx& c, const std::string& branch);

// --- showing changes ---------------------------------------------------------------------------------------------
std::string describe_line(Ctx& c, const GitCommit& cm);     // "kxyz... 1a2b3c4d main description"
bool is_empty_change(Ctx& c, const GitCommit& cm);         // no changes of its own (tree == parents' tree)

}  // namespace aios_git::change
