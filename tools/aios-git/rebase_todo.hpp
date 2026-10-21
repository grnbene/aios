#pragma once
// The rebase todo list: its lines as git writes and reads them, and the list a rebase starts from.
#include <string>
#include <vector>

#include "cli_rewrite.hpp"

namespace aios_git::rebase {

// In git's order: everything up to Squash makes a commit, Noop and after are skipped over.
enum class Op { Pick, Edit, Reword, Fixup, Squash, Exec, Break, Label, Reset, Merge, UpdateRef, Noop, Drop, Comment };

struct Item {
  Op op = Op::Noop;
  std::string oid;               // the commit a pick-like line names, or merge's -C/-c commit
  std::string arg;               // the text after the commit (its subject); the command; the label; the ref; merge's "label # oneline"
  char flag = 0;                 // 'C' or 'c': fixup -C / -c, merge -C / -c
  std::string text;              // a comment line as written
  bool commit_op() const { return op <= Op::Squash; }
};

const char* op_name(Op op);
char op_letter(Op op);

// One line of a todo list; false with `error` set (git's wording) when it is not a command.
bool parse_line(Ctx& c, const std::string& line, Item& out, std::string& error);
// The whole text: comments and blank lines are kept as Comment items. On the first bad line `error` holds git's
// messages (the reason and "invalid line N: ...") and false is returned.
bool parse_todo(Ctx& c, const std::string& text, std::vector<Item>& out, std::string& error, bool after_done = false);
// How a line is written. `shorten`: abbreviated commit names; `letters`: one-letter commands (rebase.abbreviateCommands).
std::string format_item(Ctx& c, const Item& it, bool shorten, bool letters);
std::string format_todo(Ctx& c, const std::vector<Item>& items, bool shorten, bool letters, size_t count = (size_t)-1);
// The command count the header counts (comments and blank lines are not commands).
size_t count_commands(const std::vector<Item>& items);

struct ScriptOptions {
  bool keep_empty = true;
  bool rebase_merges = false;
  bool rebase_cousins = false;
  bool reapply_cherry_picks = false;
  bool update_refs = false;
  bool autosquash = false;
  bool letters = false;
  bool root_with_onto = false;
  std::vector<std::string> exec;
  std::string onto;                // the commit the rebase replays onto
  std::string head_name;           // refs/heads/x, for update-ref
};
// The todo a rebase of `head` on `onto` starts from: the commits in `left_of..head` (all of `head` when `left_of`
// is empty), oldest first, without those already upstream, as the editor shows them (full commit names).
std::vector<Item> make_script(Ctx& c, const std::string& left_of, const std::string& head, const ScriptOptions& o);
// Moves fixup!/squash!/amend! commits after the commit they name, as --autosquash does.
void rearrange_squash(Ctx& c, std::vector<Item>& items);
// Puts `update-ref refs/heads/x` after the commit each other branch points at (--update-refs).
void add_update_ref_commands(Ctx& c, std::vector<Item>& items, const std::string& skip_ref);
void add_exec_commands(std::vector<Item>& items, const std::vector<std::string>& cmds);
// The help the editor is shown after the todo ("# Commands: ..."), with the "Rebase a..b onto c (n commands)" line.
std::string todo_help(Ctx& c, const std::string& short_range, const std::string& short_onto, size_t count, bool edit_todo);
char comment_char(Ctx& c);

}  // namespace aios_git::rebase
