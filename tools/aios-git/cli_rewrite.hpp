#pragma once
// Running programs for git (hooks, editors, credential helpers, filters, exec lines) and the commands that rewrite
// history, keep submodules and hold credentials. Shared machinery stays in cli.hpp.
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cli.hpp"

namespace aios_git {

using aios::userland::GitTreeMerge;

// ---------------------------------------------------------------------------
// programs (cli_exec.cpp)
// ---------------------------------------------------------------------------
struct ProcSpec {
  std::vector<std::string> argv;   // a program and its arguments, run as `exec "$@"` under the shell
  std::string script;              // or a shell snippet: run as `sh -c '<script> "$@"' <script> <argv...>`, as git runs one
  std::string dir;                 // working directory (absolute); empty keeps the invoker's
  std::vector<std::pair<std::string, std::string>> env;   // set in the child, over the invoker's environment
  std::vector<std::string> unset_env;
  std::optional<std::string> input;   // standard input; none is /dev/null
  bool merge_output = false;          // standard output goes where standard error does (what a hook's output does)
};
struct ProcResult {
  bool started = false;   // false: no shell, or the program could not be started
  int code = 127;
  std::string out, err;   // with merge_output, everything is in err
};
ProcResult run_proc(Ctx& c, const ProcSpec& spec);
// The environment the invoker was given plus these, for a child that should see git's own variables.
std::vector<std::pair<std::string, std::string>> child_env(Ctx& c);

// The text editor command: GIT_SEQUENCE_EDITOR then sequence.editor (when `sequence`), then GIT_EDITOR, core.editor,
// VISUAL, EDITOR. Empty when none is set.
std::string editor_command(Ctx& c, bool sequence);
// Runs the editor on `path` as git does (`sh -c '<editor> "$@"' <editor> <path>`); dies when it fails.
// The editor's own output goes where git's goes.
void edit_file(Ctx& c, const std::string& path, bool sequence, const std::string& what = "the editor");

// A hook's file when the repository has one that can run (core.hooksPath honoured), else "".
// With `warn` false a hook that exists but cannot run is not mentioned (the caller only asks whether there is one).
std::string find_hook(Ctx& c, const std::string& name, bool warn = true);
struct HookOptions {
  std::optional<std::string> input;   // standard input
  std::vector<std::pair<std::string, std::string>> env;
  bool capture_stdout = false;        // keep standard output in `out` instead of sending it to standard error
};
struct HookResult {
  bool ran = false;
  int code = 0;
  std::string out;
};
// Runs hooks/<name> with `args` from the work tree (the git dir of a bare repository) with GIT_DIR set, its output to
// standard error. Not found is `ran == false`, code 0.
HookResult run_hook(Ctx& c, const std::string& name, const std::vector<std::string>& args, const HookOptions& o = {});
// A hook that may refuse: a non-zero exit ends the command (git's own message names the hook).
void run_hook_or_die(Ctx& c, const std::string& name, const std::vector<std::string>& args, const HookOptions& o = {});
// A path inside the git directory as git hands it to a hook: ".git/COMMIT_EDITMSG" from the top of the work tree.
std::string git_dir_path_for_hook(Ctx& c, const std::string& name);
// pre-commit or pre-merge-commit (`name`), run unless `skip`: the exit status of a hook that refused, else 0.
int run_pre_commit_hook(Ctx& c, const std::string& name, bool skip, bool temp_index = false);
struct MessageHooks {
  int code = 0;           // non-zero: a hook refused
  std::string message;    // the message as the hooks left it
};
// The message file (`file`, under the git directory) goes through prepare-commit-msg (`source`, and `sha` for "commit")
// and, unless `skip_commit_msg`, commit-msg, as `git commit` and `git merge` run them.
MessageHooks run_message_hooks(Ctx& c, const std::string& file, const std::string& message, const std::string& source,
                               const std::string& sha, bool skip_commit_msg, bool temp_index = false);
// post-commit, and for an amend post-rewrite ("amend", with "<old> <new>" on standard input); their statuses do not matter.
void run_post_commit_hooks(Ctx& c, bool amend, const std::string& old_oid, const std::string& new_oid);
// post-merge, its argument 1 for a squash merge.
void run_post_merge_hook(Ctx& c, bool squash);
// post-checkout: <old> <new> <1 when a branch (or commit) was checked out, 0 for files>.
int run_post_checkout_hook(Ctx& c, const std::string& old_oid, const std::string& new_oid, bool branch);
struct RefUpdate { std::string name, old_oid, new_oid; };
// reference-transaction: "prepared" before the refs change (a refusal ends it with the hook's status), "committed" after;
// "aborted" when `apply` throws.
void with_ref_transaction(Ctx& c, const std::vector<RefUpdate>& updates, const std::function<void()>& apply);

// ---------------------------------------------------------------------------
// merging into the work tree (cmd_branch.cpp)
// ---------------------------------------------------------------------------
struct MergeApply {
  bool clean = true;
  std::vector<std::string> messages;   // Auto-merging / CONFLICT lines, in path order
  std::vector<std::string> conflicted;
};
bool apply_tree_merge(Ctx& c, const std::string& base_tree, const std::string& ours_tree, const std::string& theirs_tree,
                      const GitTreeMerge& tm, const std::string& ours_label, const std::string& theirs_label, MergeApply& out,
                      const char* what = "merge");
GitTreeMerge run_tree_merge(Ctx& c, const std::string& base, const std::string& ours, const std::string& theirs,
                            const std::string& ours_label, const std::string& theirs_label, const std::string& strategy_opt);

// ---------------------------------------------------------------------------
// commands (cmd_rebase.cpp, cmd_submodule.cpp, cmd_credential.cpp, cmd_maintenance.cpp)
// ---------------------------------------------------------------------------
int cmd_rebase(Ctx&);
int cmd_submodule(Ctx&);
int cmd_credential(Ctx&);
int cmd_maintenance(Ctx&);
// What `git status` adds for a rebase in progress ("interactive rebase in progress; onto ..." and the hints), "" when none.
std::string rebase_status_text(Ctx& c, bool in_commit_template);
bool rebase_in_progress(Ctx& c);
// `git rebase` as pull runs it, with the upstream and options already decided: returns its exit code. The reflog says
// what pull was asked (`pull --rebase origin main`), as git's does.
int rebase_for_pull(Ctx& c, const std::vector<std::string>& args, const std::string& reflog_action);
// The commit `head` branched from an earlier incarnation of `upstream_name` (git merge-base --fork-point), "" when none.
std::string rebase_fork_point(Ctx& c, const std::string& upstream_name, const std::string& head);
// require_clean_work_tree: says what is dirty, false when something is.
bool rebase_require_clean(Ctx& c, const std::string& action, const std::string& hint);

}  // namespace aios_git
