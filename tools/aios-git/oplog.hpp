#pragma once
// The operation log: every command that changes a repository is recorded, with the state it left behind, so any of
// them can be undone, redone, or the whole repository put back the way it was at any earlier point.
//
// An operation is a commit object on refs/changes/op (its parent: the operation before it). Its tree holds one blob,
// "view": HEAD, every ref (refs/changes/op itself aside) and, outside the change layer, the work tree and the index
// as trees, so a `git reset --hard` or a `git checkout -- file` that threw edits away can be undone too. Its message
// says what ran (the command line), when, by whom, and which commits it rewrote into which. Being ordinary objects,
// operations are packed, fetched and kept like any others; nothing aios-git's gc does removes what one names.
//
// Undo does not rewind: it reverts one operation's effect onto the present (a ref the operation moved goes back if
// nothing has moved it since; the files it changed are merged back three ways), so what happened after it is kept.
// Repeated undo walks further back; redo reverts the undos, newest first; a new operation ends the redo chain.
//
// On with the change layer (change.enabled), or with core.operationLog.
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "cli.hpp"

namespace aios_git::oplog {

bool enabled(Ctx& c);

struct View {
  std::string head;                              // "ref: refs/heads/x" or a commit id ("" unknown)
  std::map<std::string, std::string> refs;
  std::string worktree, index;                   // trees, outside the change layer ("" not captured)
  bool operator==(const View& o) const { return head == o.head && refs == o.refs && worktree == o.worktree && index == o.index; }
  bool same_refs(const View& o) const { return head == o.head && refs == o.refs; }
};
View capture(Ctx& c, bool with_trees);

struct Op {
  std::string id, parent;
  std::string description, kind = "normal";      // normal | snapshot | undo | redo
  std::string target;                            // the operation an undo or redo reverts
  std::string args, user;
  int64_t time = 0;
  std::vector<std::pair<std::string, std::string>> rewrites;   // old commit -> new ("" abandoned)
  View view;
};
std::optional<Op> head_op(Ctx& c);
Op read_op(Ctx& c, const std::string& id);
std::string resolve_op(Ctx& c, const std::string& spec);   // an id prefix, @ (the latest), @- ...

// Records an operation with the repository's state now. Returns its id ("" when nothing changed and nothing was
// rewritten, or the log is off).
std::string record(Ctx& c, const std::string& description, const std::string& kind, const std::string& target,
                   const std::vector<std::pair<std::string, std::string>>& rewrites);
void ensure_started(Ctx& c, const std::string& description);

// run()'s hooks around every command: before, what changed since the last operation (edits, or refs moved by
// something else) is recorded on its own; after, what the command did is recorded under its command line.
void before_command(Ctx& c);
void after_command(Ctx& c);
// Whether the command about to run may change the repository's changes or its log (and so takes the lock).
bool needs_lock(Ctx& c);

// Put the work tree's files from tree `from` to tree `to` (only the paths that differ are touched).
void apply_tree_to_disk(Ctx& c, const std::string& from, const std::string& to);

int cmd_op(Ctx& c);
int cmd_undo(Ctx& c);
int cmd_redo(Ctx& c);

}  // namespace aios_git::oplog
