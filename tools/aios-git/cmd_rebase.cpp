// rebase: replaying commits onto a new base. The state lives in .git/rebase-merge/ in git's own layout, so git and
// aios-git can pick up each other's rebases.
#include <algorithm>
#include <cctype>
#include <sstream>

#include "aios/hal/host.hpp"
#include "rebase_todo.hpp"

namespace aios_git {

using namespace aios;
using namespace aios::userland;
using namespace rebase;

namespace {
std::string status_text(Ctx& c, bool in_template, bool as_conflict);

// ---------------------------------------------------------------------------
// the state directory
// ---------------------------------------------------------------------------
std::string sdir(Ctx& c) { return c.repo->git_dir() + "/rebase-merge"; }
std::string spath(Ctx& c, const std::string& name) { return sdir(c) + "/" + name; }
std::optional<std::string> rd(Ctx& c, const std::string& name) {
  auto r = c.host().read_file(spath(c, name));
  if (!r.ok()) return std::nullopt;
  return std::string((const char*)r->data(), r->size());
}
bool has(Ctx& c, const std::string& name) { return c.host().exists(spath(c, name)); }
void wr(Ctx& c, const std::string& name, const std::string& text) {
  auto r = c.host().write_file(spath(c, name), ByteSpan((const uint8_t*)text.data(), text.size()));
  if (!r.ok()) die(c, "could not write '" + spath(c, name) + "': " + r.error().message);
}
void app(Ctx& c, const std::string& name, const std::string& text) {
  (void)c.host().append_file(spath(c, name), ByteSpan((const uint8_t*)text.data(), text.size()));
}
void rm(Ctx& c, const std::string& name) {
  if (has(c, name)) (void)c.host().remove(spath(c, name));
}
std::string first_line(const std::optional<std::string>& s) {
  if (!s) return {};
  std::string v = s->substr(0, s->find('\n'));
  while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
  return v;
}
std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> v;
  std::istringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) v.push_back(line);
  return v;
}

void remove_tree(Ctx& c, const std::string& path) {
  auto st = c.host().stat(path);
  if (!st.ok()) return;
  if (st->is_dir) {
    auto names = c.host().list_dir(path);
    if (names.ok()) for (auto& n : *names) if (n != "." && n != "..") remove_tree(c, path + "/" + n);
  }
  (void)c.host().remove(path);
}

bool in_progress(Ctx& c) { return c.host().exists(sdir(c)); }

std::string sq_quote(const std::string& s) {
  std::string o = "'";
  for (char ch : s) {
    if (ch == '\'') o += "'\\''";
    else o += ch;
  }
  return o + "'";
}

// ---------------------------------------------------------------------------
// output
// ---------------------------------------------------------------------------
int term_columns(Ctx& c) {
  std::string cols = c.getenv("COLUMNS");
  int n = cols.empty() ? 0 : std::atoi(cols.c_str());
  return n > 0 ? n : 80;
}
void clear_line(Ctx& c) {
  std::string term = c.getenv("TERM");
  if (term.empty() || term == "dumb") c.err << "\r" << std::string((size_t)term_columns(c), ' ') << "\r";
  else c.err << "\033[K";
}

// ---------------------------------------------------------------------------
// what a running rebase is made of
// ---------------------------------------------------------------------------
struct Rb {
  Ctx& c;
  bool quiet = false, verbose = false, signoff = false;
  bool drop_redundant = false, keep_redundant = false;   // neither: stop on a commit that became empty
  bool allow_ff = true, reschedule = false, cdate_is_adate = false, ignore_date = false;
  std::string strategy_opt, head_name, onto, orig_head, squash_onto;
  explicit Rb(Ctx& ctx) : c(ctx) {}
};

// A command that cannot go on and is not a conflict: the command goes back to the todo.
struct StopForError { int code; };

void reopen(Ctx& c) {
  c.repo.reset();
  c.open_repo();
}

std::string head_oid(Ctx& c) {
  auto h = read_ref(c, "HEAD");
  return h ? *h : std::string();
}

std::string reflog_msg(Ctx& c, const std::string& what, const std::string& text) {
  std::string a = c.getenv("GIT_REFLOG_ACTION");
  return (a.empty() ? "rebase" : a) + " (" + what + "): " + text;
}

// HEAD detached at a commit, as a rebase is run on one; the branch it came from stays where it is.
void detach_head(Ctx& c, const std::string& oid, const std::string& msg) {
  const std::string before = read_ref(c, "HEAD").value_or("");
  with_ref_transaction(c, {{"HEAD", before, oid}}, [&] {
    if (auto r = c.repo->set_head_detached(oid); !r.ok()) die(c, r.error().message);
    append_reflog(c, "HEAD", before, oid, msg);
  });
}
void move_head(Ctx& c, const std::string& oid, const std::string& msg) { detach_head(c, oid, msg); }

// Work tree and index to a commit's tree, the way a checkout does (local changes in the way refuse it).
bool checkout_commit(Ctx& c, const std::string& oid, std::string* why = nullptr) {
  auto r = c.repo->checkout_tree(read_commit(c, oid).tree, false);
  if (!r.ok()) {
    if (why) *why = r.error().message;
    return false;
  }
  c.repo->set_unmerged({});
  return true;
}

void hard_reset_to(Ctx& c, const std::string& oid) {
  auto r = c.repo->checkout_tree(read_commit(c, oid).tree, true);
  if (!r.ok()) die(c, "Could not reset index file to revision '" + oid + "'.");
  c.repo->set_unmerged({});
  if (auto w = c.repo->write_index(); !w.ok()) die(c, w.error().message);
}

bool has_unstaged(Ctx& c) {
  auto wt = c.repo->worktree_changes();
  if (!wt.ok()) return false;
  for (auto& ch : *wt) if (ch.change != GitChange::Untracked) return true;
  return false;
}

bool has_staged(Ctx& c) {
  if (!c.repo->unmerged().empty()) return true;
  std::string head = head_oid(c);
  Snapshot h = snapshot_tree(c, head.empty() ? "" : read_commit(c, head).tree);
  Snapshot i = snapshot_index(c);
  if (h.size() != i.size()) return true;
  for (auto& [p, f] : i) {
    auto it = h.find(p);
    if (it == h.end() || it->second.oid != f.oid || it->second.mode != f.mode) return true;
  }
  return false;
}

// git's require_clean_work_tree for `rebase`.
bool require_clean(Ctx& c, const std::string& action, const std::string& hint) {
  bool err = false;
  if (has_unstaged(c)) {
    c.err << "error: cannot " << action << ": You have unstaged changes.\n";
    err = true;
  }
  if (has_staged(c)) {
    if (err) c.err << "error: additionally, your index contains uncommitted changes.\n";
    else c.err << "error: cannot " << action << ": Your index contains uncommitted changes.\n";
    err = true;
  }
  if (err && !hint.empty()) c.err << "error: " << hint << "\n";
  return !err;
}

// ---------------------------------------------------------------------------
// author-script: GIT_AUTHOR_NAME='..' lines
// ---------------------------------------------------------------------------
std::string author_script_of(const GitCommit& cm) {
  std::string name = cm.author.substr(0, cm.author.find(" <"));
  size_t lt = cm.author.find('<'), gt = cm.author.find('>');
  std::string email = lt == std::string::npos || gt == std::string::npos ? "" : cm.author.substr(lt + 1, gt - lt - 1);
  return "GIT_AUTHOR_NAME=" + sq_quote(name) + "\nGIT_AUTHOR_EMAIL=" + sq_quote(email) + "\nGIT_AUTHOR_DATE=" +
         sq_quote("@" + std::to_string(cm.author_time) + " " + cm.author_tz) + "\n";
}

std::string author_of(const GitCommit& cm) { return cm.author + " " + std::to_string(cm.author_time) + " " + cm.author_tz; }

// "Name <email> seconds tz" from an author-script, "" when it does not read.
std::string author_from_script_text(const std::string& text) {
  std::map<std::string, std::string> kv;
  for (auto& line : lines_of(text)) {
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string v = line.substr(eq + 1), out;
    if (!v.empty() && v[0] == '\'') {
      size_t i = 1;
      while (i < v.size()) {
        if (v[i] == '\'') {
          if (v.compare(i, 4, "'\\''") == 0) {
            out += '\'';
            i += 4;
            continue;
          }
          break;
        }
        out += v[i++];
      }
    } else {
      out = v;
    }
    kv[line.substr(0, eq)] = out;
  }
  if (!kv.count("GIT_AUTHOR_NAME") || !kv.count("GIT_AUTHOR_EMAIL") || !kv.count("GIT_AUTHOR_DATE")) return {};
  std::string date = kv["GIT_AUTHOR_DATE"];
  if (!date.empty() && date[0] == '@') date = date.substr(1);
  return kv["GIT_AUTHOR_NAME"] + " <" + kv["GIT_AUTHOR_EMAIL"] + "> " + date;
}

// ---------------------------------------------------------------------------
// commits
// ---------------------------------------------------------------------------
// The author a rebased commit keeps (its own; --reset-author-date gives it today's date) and the committer.
std::string author_for(Rb& rb, const GitCommit& cm) {
  if (!rb.ignore_date) return author_of(cm);
  std::string now = rb.c.ident(false);
  return cm.author + now.substr(now.find('>') + 1);
}

std::string committer_for(Rb& rb, const std::string& author) {
  std::string who = rb.c.ident(true);
  if (rb.cdate_is_adate) {
    size_t gt = who.find('>'), ag = author.find('>');
    if (gt != std::string::npos && ag != std::string::npos) who = who.substr(0, gt + 1) + author.substr(ag + 1);
  }
  return who;
}

std::string first_line_of(const std::string& msg) { return msg.substr(0, msg.find('\n')); }

// A message on its way into a commit of the rebase goes through prepare-commit-msg, as git's commits made from a todo do.
std::string through_prepare_hook(Rb& rb, const std::string& msg) {
  MessageHooks mh = run_message_hooks(rb.c, "COMMIT_EDITMSG", msg, "message", "", true);
  if (mh.code != 0) throw StopForError{1};
  return mh.message;
}

std::string add_signoff(Ctx& c, std::string msg) {
  std::string me = c.ident(true);
  std::string line = "Signed-off-by: " + me.substr(0, me.find('>') + 1);
  if (msg.find(line) != std::string::npos) return msg;
  if (msg.empty() || msg.back() != '\n') msg += "\n";
  size_t last_para = msg.rfind("\n\n", msg.size() >= 2 ? msg.size() - 2 : 0);
  std::string last = last_para == std::string::npos ? msg : msg.substr(last_para + 2);
  bool trailers = last_para != std::string::npos && last.find(": ") != std::string::npos;
  return msg + (trailers ? "" : "\n") + line + "\n";
}

std::string commented_lines(Ctx& c, const std::string& text) {
  const char cc = comment_char(c);
  std::string out;
  for (auto& line : lines_of(text))
    out += line.empty() ? std::string(1, cc) + "\n" : std::string(1, cc) + (line[0] == '\t' ? "" : " ") + line + "\n";
  return out;
}

// The staged changes against `base_tree`, as a commit template lists them.
std::string staged_listing(Ctx& c, const std::string& base_tree) {
  Snapshot a = snapshot_tree(c, base_tree), b = snapshot_index(c);
  std::string out;
  for (auto& [p, f] : b) {
    auto it = a.find(p);
    if (it == a.end()) out += "\tnew file:   " + p + "\n";
    else if (it->second.oid != f.oid || it->second.mode != f.mode) out += "\tmodified:   " + p + "\n";
  }
  for (auto& [p, f] : a) if (!b.count(p)) out += "\tdeleted:    " + p + "\n";
  return out.empty() ? out : "Changes to be committed:\n" + out;
}

// What a commit message template says about the commit being made.
struct EditCtx {
  std::string author, date;   // "Name <email>", the date line's text (none: no Date line)
  std::string base_tree;      // the changes listed are the index against this
  bool rebasing = false;      // the state is described as a rebase with conflicts (MERGE_MSG there), not an edit
};
EditCtx edit_ctx_of(const GitCommit& cm, const std::string& base_tree, bool rebasing = false) {
  return EditCtx{cm.author, format_date((int64_t)cm.author_time, cm.author_tz, "default"), base_tree, rebasing};
}
std::string parent_tree_of(Ctx& c, const GitCommit& cm) { return cm.parents.empty() ? std::string() : read_commit(c, cm.parents[0]).tree; }

// The message an editor session starts from and what comes back of it; empty aborts, as commit does.
std::string edit_message(Rb& rb, const std::string& msg, const EditCtx& e) {
  Ctx& c = rb.c;
  const char cc = comment_char(c);
  std::string text = cleanup_message(msg, false);
  if (text.empty() || text.back() != '\n') text += "\n";
  text += "\n";
  text += commented_lines(c, "Please enter the commit message for your changes. Lines starting\nwith '" + std::string(1, cc) +
                                 "' will be ignored, and an empty message aborts the commit.");
  text += std::string(1, cc) + "\n";
  if (!e.author.empty()) {
    text += commented_lines(c, "Author:    " + e.author + (e.date.empty() ? "" : "\nDate:      " + e.date));
    text += std::string(1, cc) + "\n";
  }
  std::string state = status_text(c, true, e.rebasing);
  if (!state.empty()) text += commented_lines(c, state);
  std::string listing = staged_listing(c, e.base_tree);
  if (!listing.empty()) text += commented_lines(c, listing);
  text += std::string(1, cc) + "\n";
  c.write_git_file("COMMIT_EDITMSG", text);
  if (editor_command(c, false) != ":") edit_file(c, c.repo->git_dir() + "/COMMIT_EDITMSG", false);
  reopen(c);
  auto back = c.read_git_file("COMMIT_EDITMSG");
  return cleanup_message(back.value_or(text), true);
}

// ---------------------------------------------------------------------------
// the todo on disk
// ---------------------------------------------------------------------------
bool letters_on(Ctx& c) { return c.config.get_bool("rebase.abbreviatecommands", false); }

std::vector<Item> read_todo(Ctx& c, bool& ok, std::string* error = nullptr) {
  std::vector<Item> items;
  std::string err;
  auto t = rd(c, "git-rebase-todo");
  ok = true;
  if (!t) return items;
  ok = parse_todo(c, *t, items, err, has(c, "done"));
  if (!ok && error) *error = err;
  return items;
}

// The text as `stripspace` leaves it: no comment lines, no trailing blanks, runs of empty lines made one, none at the ends.
std::string strip_todo_space(Ctx& c, const std::string& text) {
  std::string out;
  bool blank_pending = false;
  for (auto line : lines_of(text)) {
    if (!line.empty() && line[0] == comment_char(c)) continue;
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();
    if (line.empty()) {
      blank_pending = !out.empty();
      continue;
    }
    if (blank_pending) out += "\n";
    blank_pending = false;
    out += line + "\n";
  }
  return out;
}

void write_todo(Ctx& c, const std::vector<Item>& items) { wr(c, "git-rebase-todo", format_todo(c, items, false, false)); }

std::vector<std::string> done_lines(Ctx& c) {
  std::vector<std::string> v;
  auto d = rd(c, "done");
  if (!d) return v;
  for (auto& line : lines_of(*d)) if (!line.empty() && line[0] != comment_char(c)) v.push_back(line);
  return v;
}

// Whether the next command (comments and blank lines do not count) is a squash or a fixup.
bool next_is_fixup(const std::vector<Item>& todo, size_t at) {
  while (at < todo.size() && todo[at].op == Op::Comment) ++at;
  return at < todo.size() && (todo[at].op == Op::Fixup || todo[at].op == Op::Squash);
}

// ---------------------------------------------------------------------------
// the rewritten list: which commit became which (for post-rewrite)
// ---------------------------------------------------------------------------
void record_in_rewritten(Rb& rb, const std::string& old_oid, bool next_fixup) {
  Ctx& c = rb.c;
  app(c, "rewritten-pending", old_oid + "\n");
  if (next_fixup) return;
  auto pending = rd(c, "rewritten-pending");
  if (!pending) return;
  std::string head = head_oid(c), out;
  for (auto& line : lines_of(*pending)) if (line.size() >= 40) out += line.substr(0, 40) + " " + head + "\n";
  app(c, "rewritten-list", out);
  rm(c, "rewritten-pending");
}

// ---------------------------------------------------------------------------
// stopping
// ---------------------------------------------------------------------------
std::string patch_text(Ctx& c, const GitCommit& cm) {
  std::string from = cm.parents.empty() ? "" : read_commit(c, cm.parents[0]).tree;
  DiffOptions o;
  o.format = DiffOptions::Patch;
  auto pairs = diff_trees(c, from, cm.tree, Pathspec{}, o);
  std::ostringstream ss;
  print_diff(c, pairs, o, ss);
  return ss.str();
}

std::string gpg_quoted(Rb& rb) { return rb.c.sign == 1 ? (rb.c.sign_key.empty() ? "-S" : "-S" + rb.c.sign_key) : ""; }

// What an edit or a conflict leaves for the person to look at: REBASE_HEAD, the patch, the message.
void write_stop_files(Rb& rb, const GitCommit& cm, const std::string& message, bool verbatim = false) {
  Ctx& c = rb.c;
  c.write_git_file("REBASE_HEAD", cm.oid + "\n");
  wr(c, "patch", patch_text(c, cm));
  wr(c, "stopped-sha", cm.oid + "\n");
  wr(c, "author-script", author_script_of(cm));
  wr(c, "message", verbatim ? message : message + "\n");
}

[[noreturn]] void stopped_for_amend(Rb& rb, const GitCommit& cm, const std::string& subject) {
  Ctx& c = rb.c;
  write_stop_files(rb, cm, cm.raw_message);
  wr(c, "amend", head_oid(c) + "\n");
  if (!rb.verbose) clear_line(c);
  c.err << "Stopped at " << c.abbrev(cm.oid) << "...  " << subject << "\n";
  c.err << "You can amend the commit now, with\n\n  git commit --amend " << gpg_quoted(rb)
        << "\n\nOnce you are satisfied with your changes, run\n\n  git rebase --continue\n";
  throw Exit{0};
}

const char* const kConflictHint =
    "hint: Resolve all conflicts manually, mark them as resolved with\n"
    "hint: \"git add/rm <conflicted_files>\", then run \"git rebase --continue\".\n"
    "hint: You can instead skip this commit: run \"git rebase --skip\".\n"
    "hint: To abort and get back to the state before \"git rebase\", run \"git rebase --abort\".\n";

[[noreturn]] void stopped_conflict(Rb& rb, const GitCommit& cm, const std::string& subject, const std::string& message,
                                   bool verbatim = false) {
  Ctx& c = rb.c;
  write_stop_files(rb, cm, message, verbatim);
  c.err << "Could not apply " << c.abbrev(cm.oid) << "... " << subject << "\n";
  throw Exit{1};
}

// ---------------------------------------------------------------------------
// squash and fixup messages
// ---------------------------------------------------------------------------
size_t fixup_count(Ctx& c) {
  auto f = rd(c, "current-fixups");
  if (!f) return 0;
  size_t n = 0;
  for (auto& line : lines_of(*f)) if (!line.empty()) ++n;
  return n;
}

bool seen_squash(Ctx& c) {
  auto f = rd(c, "current-fixups");
  if (!f) return false;
  for (auto& line : lines_of(*f)) if (line.rfind("squash", 0) == 0) return true;
  return false;
}

void update_squash_messages(Rb& rb, const Item& item, const GitCommit& commit) {
  Ctx& c = rb.c;
  const char cc = comment_char(c);
  const bool fixup_flag = item.op == Op::Fixup && item.flag != 0;
  std::string buf;
  const size_t count = fixup_count(c);
  if (count > 0) {
    buf = rd(c, "message-squash").value_or("");
    if (buf.empty()) die(c, "could not read '" + spath(c, "message-squash") + "'");
    size_t eol = buf[0] == cc ? buf.find('\n') : 0;
    if (eol == std::string::npos) eol = buf.size();
    buf = std::string(1, cc) + " This is a combination of " + std::to_string(count + 2) + " commits." + buf.substr(eol);
    if (fixup_flag && !seen_squash(c)) {
      // -C / -c: the first message becomes a comment, this commit's message the one that is kept
      size_t first = buf.find("\n\n");
      if (first != std::string::npos) {
        size_t next = buf.find("\n" + std::string(1, cc) + " ", first);
        std::string head = buf.substr(0, first + 2);
        std::string body = buf.substr(first + 2, next == std::string::npos ? std::string::npos : next - first - 2);
        std::string tail = next == std::string::npos ? "" : buf.substr(next);
        size_t what = head.find("This is the 1st commit message:");
        if (what != std::string::npos) head.replace(what, 31, "The first commit's message is:");
        buf = head + commented_lines(c, body) + tail;
      }
    }
  } else {
    std::string head = head_oid(c);
    if (head.empty()) die(c, "need a HEAD to fixup");
    GitCommit hc = read_commit(c, head);
    if (item.op == Op::Fixup && !fixup_flag) wr(c, "message-fixup", hc.raw_message);
    buf = std::string(1, cc) + " This is a combination of 2 commits.\n" + std::string(1, cc) + " " +
          (fixup_flag ? "The first commit's message is:" : "This is the 1st commit message:") + "\n\n";
    buf += fixup_flag ? commented_lines(c, hc.raw_message) : hc.raw_message;
  }
  const std::string& body = commit.raw_message;
  if (item.op == Op::Squash || fixup_flag) {
    // the subject of an amend!, and of a squash!/fixup! once a squash is in the chain, is not part of the message
    size_t commented_len = 0;
    const bool squashing = item.op == Op::Squash || seen_squash(c);
    if (body.rfind("amend!", 0) == 0 || (squashing && (body.rfind("squash!", 0) == 0 || body.rfind("fixup!", 0) == 0))) {
      size_t at = 0;
      while (at < body.size()) {
        size_t nl = body.find('\n', at);
        if (nl == at) break;
        at = nl == std::string::npos ? body.size() : nl + 1;
      }
      commented_len = at;
    }
    buf += "\n" + std::string(1, cc) + " This is the commit message #" + std::to_string(count + 2) + ":\n\n" +
           commented_lines(c, body.substr(0, commented_len)) + body.substr(commented_len);
    if (fixup_flag && !seen_squash(c)) wr(c, "message-fixup", body.substr(commented_len));
  } else {
    buf += "\n" + std::string(1, cc) + " The commit message #" + std::to_string(count + 2) + " will be skipped:\n\n" + commented_lines(c, body);
  }
  wr(c, "message-squash", buf);
  std::string cur = rd(c, "current-fixups").value_or("");
  if (!cur.empty() && cur.back() == '\n') cur.pop_back();
  wr(c, "current-fixups", cur + (cur.empty() ? "" : "\n") + op_name(item.op) + " " + commit.oid);
}

void clear_squash_state(Ctx& c) {
  for (const char* f : {"message-squash", "message-fixup", "current-fixups"}) rm(c, f);
  c.remove_git_file("SQUASH_MSG");
}

// The squash or fixup `item` is in the index: HEAD is made again with the combined message. The last of a chain
// finishes the message (through the editor when a squash was in it).
void commit_fixup(Rb& rb, const Item& item, bool final_fixup, bool resumed = false) {
  Ctx& c = rb.c;
  const std::string head = head_oid(c);
  GitCommit hc = read_commit(c, head);
  std::string message;
  bool edit = false;
  if (!final_fixup) {
    message = rd(c, "message-squash").value_or("");
    edit = resumed;
  } else if (has(c, "message-fixup") && !seen_squash(c) && item.flag != 'c') {
    message = rd(c, "message-fixup").value_or("");
  } else {
    message = rd(c, "message-squash").value_or("");
    edit = true;
  }
  if (edit) {
    c.write_git_file("SQUASH_MSG", message);
    message = edit_message(rb, message, edit_ctx_of(hc, parent_tree_of(c, hc), true));
    if (message.empty()) {
      c.err << "Aborting commit due to empty commit message.\n";
      wr(c, "amend", head + "\n");
      throw StopForError{1};
    }
  }
  std::string tree = write_tree_of(c, c.repo->index());
  std::string a = author_of(hc);
  std::string msg = through_prepare_hook(rb, rb.signoff ? add_signoff(c, message) : message);
  std::string oid = make_commit(c, tree, hc.parents, msg, a, committer_for(rb, a), true);
  move_head(c, oid, reflog_msg(c, resumed ? "continue" : item.op == Op::Fixup ? "fixup" : "squash", first_line_of(msg)));
  run_post_commit_hooks(c, false, "", oid);
  if (resumed || !edit) c.remove_git_file("REBASE_HEAD");   // one made through the editor on a pick leaves it
  if ((resumed || edit) && !rb.quiet) print_new_commit(c, oid, true, hc.parents.empty());
  if (final_fixup || resumed) clear_squash_state(c);
}

// ---------------------------------------------------------------------------
// one commit-making command
// ---------------------------------------------------------------------------
// Rewords HEAD: its message through the editor, the commit made again.
void reword_head(Rb& rb) {
  Ctx& c = rb.c;
  GitCommit hc = read_commit(c, head_oid(c));
  std::string msg = edit_message(rb, hc.raw_message, edit_ctx_of(hc, parent_tree_of(c, hc)));
  if (msg.empty()) {
    c.err << "Aborting commit due to empty commit message.\n";
    throw StopForError{1};
  }
  std::string tree = write_tree_of(c, c.repo->index());
  std::string a = author_of(hc);
  msg = through_prepare_hook(rb, msg);
  std::string oid = make_commit(c, tree, hc.parents, msg, a, committer_for(rb, a), true);
  move_head(c, oid, reflog_msg(c, "reword", first_line_of(msg)));
  run_post_commit_hooks(c, false, "", oid);
  if (!rb.quiet) print_new_commit(c, oid, true, hc.parents.empty());
}

// The commit's change merged onto HEAD, into the index and work tree.
struct Merged {
  bool conflict = false;
};
Merged merge_commit(Rb& rb, const GitCommit& cm, const std::string& head, const std::string& subject) {
  Ctx& c = rb.c;
  Merged r;
  std::string parent_tree = cm.parents.empty() ? empty_tree_oid() : read_commit(c, cm.parents[0]).tree;
  std::string ours_tree = read_commit(c, head).tree;
  std::string theirs_label = c.abbrev(cm.oid) + " (" + subject + ")";
  GitTreeMerge tm = run_tree_merge(c, parent_tree, ours_tree, cm.tree, "HEAD", theirs_label, rb.strategy_opt);
  MergeApply ap;
  if (!apply_tree_merge(c, parent_tree, ours_tree, cm.tree, tm, "HEAD", theirs_label, ap, "merge")) throw StopForError{1};
  r.conflict = !ap.clean;
  // a clean merge says nothing; a conflicted one tells what it found
  if (r.conflict) for (auto& m : ap.messages) c.out << m << "\n";
  if (r.conflict) {
    std::string mm = cm.raw_message;
    if (mm.empty() || mm.back() != '\n') mm += "\n";
    mm += "\n# Conflicts:\n";
    for (auto& f : ap.conflicted) mm += "#\t" + f + "\n";
    c.write_git_file("MERGE_MSG", mm);
  }
  return r;
}

// pick, reword, edit, squash, fixup.
void do_pick(Rb& rb, const Item& item, bool final_fixup) {
  Ctx& c = rb.c;
  GitCommit cm = read_commit(c, item.oid);
  const std::string subject = first_line_of(cm.raw_message);
  const std::string todo_subject = item.arg.empty() ? subject_of(cm.message) : item.arg;
  const bool is_fixup = item.op == Op::Fixup || item.op == Op::Squash;
  const std::string head = head_oid(c);
  if (cm.parents.size() > 1) {
    c.err << "error: commit " << cm.oid << " is a merge but no -m option was given.\n";
    throw StopForError{1};
  }
  const std::string parent = cm.parents.empty() ? "" : cm.parents[0];
  for (const char* f : {"message", "author-script", "stopped-sha"}) rm(c, f);
  c.remove_git_file("CHERRY_PICK_HEAD");
  if (rb.allow_ff && !is_fixup && ((!parent.empty() && parent == head) || (parent.empty() && head == rb.squash_onto))) {
    std::string why;
    if (!checkout_commit(c, cm.oid, &why)) {
      c.err << "error: " << why << "\nAborting\n";
      throw StopForError{1};
    }
    std::string action = c.getenv("GIT_REFLOG_ACTION");
    move_head(c, cm.oid, item.op == Op::Edit ? reflog_msg(c, "edit", subject) : (action.empty() ? "rebase" : action) + ": fast-forward");
    if (item.op == Op::Reword) reword_head(rb);
    return;
  }
  c.write_git_file("REBASE_HEAD", cm.oid + "\n");
  if (is_fixup) update_squash_messages(rb, item, cm);
  if (merge_commit(rb, cm, head, subject).conflict) {
    std::string message = cm.raw_message;
    if (is_fixup) {
      message = rd(c, "message-squash").value_or("");
      c.write_git_file("MERGE_MSG", message);
      wr(c, "amend", head + "\n");
    }
    c.err << "error: could not apply " << c.abbrev(cm.oid) << "... " << subject << "\n" << kConflictHint;
    stopped_conflict(rb, cm, todo_subject, message, is_fixup);
  }
  if (is_fixup) {
    commit_fixup(rb, item, final_fixup);
    return;
  }
  const std::string tree = write_tree_of(c, c.repo->index());
  const bool originally_empty = cm.tree == (parent.empty() ? empty_tree_oid() : read_commit(c, parent).tree);
  if (tree == read_commit(c, head).tree && !originally_empty && !rb.keep_redundant) {
    if (rb.drop_redundant) {
      c.remove_git_file("REBASE_HEAD");
      c.err << "dropping " << cm.oid << " " << subject << " -- patch contents already upstream\n";
      return;
    }
    write_stop_files(rb, cm, cm.raw_message);
    c.write_git_file("MERGE_MSG", cm.raw_message);
    c.write_git_file("CHERRY_PICK_HEAD", cm.oid + "\n");
    c.err << "The previous cherry-pick is now empty, possibly due to conflict resolution.\n"
             "If you wish to commit it anyway, use:\n\n    git commit --allow-empty\n\n"
             "Otherwise, please use 'git rebase --skip'\n"
          << rebase_status_text(c, false) << "nothing to commit, working tree clean\n";
    stopped_conflict(rb, cm, todo_subject, cm.raw_message);
  }
  std::string msg = cm.raw_message;
  if (item.op == Op::Reword) {
    msg = edit_message(rb, msg, edit_ctx_of(cm, read_commit(c, head).tree));
    if (msg.empty()) {
      c.err << "Aborting commit due to empty commit message.\n";
      throw StopForError{1};
    }
  }
  if (rb.signoff) msg = add_signoff(c, msg);
  msg = through_prepare_hook(rb, msg);
  std::string a = author_for(rb, cm);
  std::string oid = make_commit(c, tree, {head}, msg, a, committer_for(rb, a), true);
  c.remove_git_file("REBASE_HEAD");
  move_head(c, oid, reflog_msg(c, op_name(item.op), item.op == Op::Reword ? first_line_of(msg) : subject));
  run_post_commit_hooks(c, false, "", oid);
  if (item.op == Op::Reword && !rb.quiet) print_new_commit(c, oid, true, false);
}

// ---------------------------------------------------------------------------
// exec, labels, resets, merges, update-ref
// ---------------------------------------------------------------------------
bool do_exec(Rb& rb, const Item& item) {
  Ctx& c = rb.c;
  clear_line(c);
  c.err << "Executing: " << item.arg << "\n";
  c.err.flush();
  c.out.flush();
  ProcSpec s;
  s.script = item.arg;
  s.dir = c.top;
  s.env = {{"GIT_DIR", c.repo->git_dir() == c.top + "/.git" ? ".git" : c.repo->git_dir()}};
  ProcResult r = run_proc(c, s);
  c.out << r.out;
  c.err << r.err;
  reopen(c);
  if (!r.started) c.err << "error: could not run '" << item.arg << "'\n";
  return r.started && r.code == 0;
}

std::string label_ref(const std::string& name) {
  std::string n;
  for (char ch : name) {
    if (std::isspace((unsigned char)ch) || ch == '~' || ch == '^' || ch == ':' || ch == '?' || ch == '*' || ch == '[' || ch == '\\') n += '-';
    else n += ch;
  }
  return "refs/rewritten/" + n;
}

void do_label(Rb& rb, const Item& item) {
  Ctx& c = rb.c;
  std::string ref = label_ref(item.arg);
  if (item.arg.rfind("#", 0) == 0 || !Git::valid_ref_name(ref)) {
    c.err << "error: '" << item.arg << "' is not a valid label\n";
    throw StopForError{1};
  }
  auto r = c.repo->update_ref(ref, head_oid(c));
  if (!r.ok()) {
    c.err << "error: could not write ref '" << ref << "'\n";
    throw StopForError{1};
  }
  app(c, "refs-to-delete", ref + "\n");
}

void do_reset(Rb& rb, const Item& item_in) {
  Ctx& c = rb.c;
  Item item = item_in;
  item.arg = item.arg.substr(0, item.arg.find(' '));   // "name # oneline": the oneline is only a reminder
  if (item.arg == "[new root]") {
    c.err << "error: aios-git does not start a new root commit from a todo line\n";
    throw StopForError{1};
  }
  if (has_unstaged(c) || has_staged(c)) {
    c.err << "error: cannot reset: Your index contains uncommitted changes.\n";
    throw StopForError{1};
  }
  auto r = read_ref(c, label_ref(item.arg));
  if (!r) r = resolve(c, item.arg + "^{commit}");
  if (!r) {
    c.err << "error: could not resolve '" << item.arg << "'\n";
    throw StopForError{1};
  }
  std::string why;
  if (!checkout_commit(c, *r, &why)) {
    c.err << "error: " << why << "\n";
    throw StopForError{1};
  }
  if (*r != head_oid(c)) move_head(c, *r, reflog_msg(c, "reset", "'" + item.arg + "'"));
}

void do_merge(Rb& rb, const Item& item) {
  Ctx& c = rb.c;
  std::string spec = item.arg, comment;
  size_t hash = spec.find(" # ");
  if (hash != std::string::npos) {
    comment = spec.substr(hash + 3);
    spec = spec.substr(0, hash);
  }
  std::vector<std::string> labels;
  std::istringstream ss(spec);
  std::string w;
  while (ss >> w) labels.push_back(w);
  if (labels.empty()) {
    c.err << "error: merge needs a label\n";
    throw StopForError{1};
  }
  if (labels.size() > 1) {
    c.err << "error: aios-git does not replay octopus merges\n";
    throw StopForError{1};
  }
  const std::string head = head_oid(c);
  std::string theirs = read_ref(c, label_ref(labels[0])).value_or("");
  if (theirs.empty()) {
    auto any = resolve(c, labels[0] + "^{commit}");
    if (!any) {
      c.err << "error: could not resolve '" << labels[0] << "'\n";
      throw StopForError{1};
    }
    theirs = *any;
  }
  std::string message = item.flag ? read_commit(c, item.oid).raw_message : (comment.empty() ? "Merge " + labels[0] : comment);
  if (message.empty() || message.back() != '\n') message += "\n";
  if (theirs == head || is_ancestor(c, theirs, head)) return;
  auto bases = merge_bases(c, head, theirs);
  std::string ours_tree = read_commit(c, head).tree, theirs_tree = read_commit(c, theirs).tree;
  std::string base_tree = bases.empty() ? "" : read_commit(c, bases[0]).tree;
  GitTreeMerge tm = run_tree_merge(c, base_tree, ours_tree, theirs_tree, "HEAD", labels[0], rb.strategy_opt);
  MergeApply ap;
  if (!apply_tree_merge(c, base_tree, ours_tree, theirs_tree, tm, "HEAD", labels[0], ap, "merge")) throw StopForError{1};
  if (!ap.clean) for (auto& m : ap.messages) c.out << m << "\n";
  if (!ap.clean) {
    std::string mm = message + "\n# Conflicts:\n";
    for (auto& f : ap.conflicted) mm += "#\t" + f + "\n";
    c.write_git_file("MERGE_MSG", mm);
    c.write_git_file("MERGE_HEAD", theirs + "\n");
    c.write_git_file("MERGE_MODE", "no-ff");
    c.err << "error: could not merge '" << labels[0] << "'\n";
    throw Exit{1};
  }
  std::string author;
  if (item.flag) {
    GitCommit orig = read_commit(c, item.oid);
    author = author_of(orig);
    if (item.flag == 'c') message = edit_message(rb, message, edit_ctx_of(orig, read_commit(c, head).tree));
  }
  std::string oid = make_commit(c, write_tree_of(c, c.repo->index()), {head, theirs}, message, author,
                                author.empty() ? std::string() : committer_for(rb, author), true);
  move_head(c, oid, reflog_msg(c, "pick", subject_of(message)));
}

// update-refs: three lines per ref (name, value before, value now); a ref named again keeps its place.
void do_update_ref(Rb& rb, const Item& item) {
  Ctx& c = rb.c;
  std::vector<std::string> lines = lines_of(rd(c, "update-refs").value_or(""));
  bool found = false;
  for (size_t i = 0; i + 2 < lines.size(); i += 3) {
    if (lines[i] == item.arg) {
      lines[i + 2] = head_oid(c);
      found = true;
    }
  }
  if (!found) {
    auto cur = read_ref(c, item.arg);
    lines.push_back(item.arg);
    lines.push_back(cur.value_or(std::string(40, '0')));
    lines.push_back(head_oid(c));
  }
  std::string out;
  for (auto& l : lines) out += l + "\n";
  wr(c, "update-refs", out);
}

// ---------------------------------------------------------------------------
// the stash that --autostash keeps the local changes in (not in the stash list, as git's)
// ---------------------------------------------------------------------------
std::string create_autostash(Ctx& c) {
  std::string head = head_oid(c);
  if (head.empty()) return {};
  GitCommit hc = read_commit(c, head);
  std::string itree = write_tree_of(c, c.repo->index());
  std::vector<GitIndexEntry> wentries;
  auto wt = c.repo->worktree_changes();
  if (!wt.ok()) die(c, wt.error().message);
  std::map<std::string, GitChange> changed;
  for (auto& ch : *wt) changed[ch.path] = ch.change;
  for (auto& e : c.repo->index()) {
    auto it = changed.find(e.path);
    if (it == changed.end()) { wentries.push_back(e); continue; }
    if (it->second == GitChange::Deleted) continue;
    auto s = c.repo->stage_entry(e.path, &e);
    if (!s.ok()) die(c, s.error().message);
    wentries.push_back(*s);
  }
  std::string wtree = write_tree_of(c, wentries);
  if (itree == hc.tree && wtree == hc.tree) return {};
  std::string b = c.repo->branch();
  std::string where = (b.empty() ? "(no branch)" : b) + ": " + c.abbrev(head) + " " + subject_of(hc.message);
  std::string icommit = make_commit(c, itree, {head}, "index on " + where + "\n");
  return make_commit(c, wtree, {head, icommit}, "On " + (b.empty() ? std::string("(no branch)") : b) + ": autostash");
}

// Applies a stash commit onto the work tree; the changes come back unstaged. False when it conflicted.
bool apply_stash_commit(Ctx& c, const std::string& oid) {
  GitCommit w = read_commit(c, oid);
  if (w.parents.empty()) die(c, oid + " is not a stash-like commit");
  std::string base_tree = read_commit(c, w.parents[0]).tree;
  std::string ours_tree = write_tree_of(c, c.repo->index());
  GitTreeMerge tm = run_tree_merge(c, base_tree, ours_tree, w.tree, "Updated upstream", "Stashed changes", "");
  MergeApply ap;
  std::vector<GitIndexEntry> before = c.repo->index();
  if (!apply_tree_merge(c, base_tree, ours_tree, w.tree, tm, "Updated upstream", "Stashed changes", ap)) return false;
  if (ap.clean) {
    std::map<std::string, GitIndexEntry> was;
    for (auto& e : before) was[e.path] = e;
    std::vector<GitIndexEntry> idx;
    for (auto& e : c.repo->index()) {
      auto it = was.find(e.path);
      idx.push_back(it != was.end() ? it->second : e);
    }
    std::set<std::string> have;
    for (auto& e : idx) have.insert(e.path);
    for (auto& e : before) if (!have.count(e.path)) idx.push_back(e);
    c.repo->set_index(idx);
    if (auto wr2 = c.repo->write_index(); !wr2.ok()) die(c, wr2.error().message);
  }
  return ap.clean;
}

// Puts a stash commit where `git stash pop` finds it.
void store_stash(Ctx& c, const std::string& oid) {
  std::string prev = read_ref(c, "refs/stash").value_or("");
  if (auto r = c.repo->update_ref("refs/stash", oid); !r.ok()) die(c, r.error().message);
  append_reflog(c, "refs/stash", prev, oid, "autostash");
}

void apply_autostash(Rb& rb) {
  Ctx& c = rb.c;
  std::string oid = first_line(rd(c, "autostash"));
  rm(c, "autostash");
  if (oid.size() != 40) return;
  if (apply_stash_commit(c, oid)) {
    c.err << "Applied autostash.\n";
    return;
  }
  store_stash(c, oid);
  c.err << "Applying autostash resulted in conflicts.\nYour changes are safe in the stash.\nYou can run \"git stash pop\" or \"git stash drop\" at any time.\n";
}

// ---------------------------------------------------------------------------
// the loop
// ---------------------------------------------------------------------------
void load_opts(Rb& rb) {
  Ctx& c = rb.c;
  rb.head_name = first_line(rd(c, "head-name"));
  rb.onto = first_line(rd(c, "onto"));
  rb.orig_head = first_line(rd(c, "orig-head"));
  rb.squash_onto = first_line(rd(c, "squash-onto"));
  rb.quiet = has(c, "quiet");
  rb.verbose = has(c, "verbose");
  rb.signoff = has(c, "signoff");
  rb.drop_redundant = has(c, "drop_redundant_commits");
  rb.keep_redundant = has(c, "keep_redundant_commits");
  rb.reschedule = has(c, "reschedule-failed-exec");
  std::string so = first_line(rd(c, "strategy_opts"));
  size_t at = so.find("--");
  if (at != std::string::npos) so = so.substr(at + 2);
  if (so.size() >= 2 && so.front() == '\'' && so.back() == '\'') so = so.substr(1, so.size() - 2);
  rb.strategy_opt = so;
  std::string g = first_line(rd(c, "gpg_sign_opt"));
  if (g.rfind("-S", 0) == 0) {
    c.sign = 1;
    c.sign_key = g.substr(2);
  }
  if (c.config.get_bool("rebase.committerdateisauthordate", false)) rb.cdate_is_adate = true;
}

void remove_state(Ctx& c, bool leave_work = false) {
  remove_tree(c, sdir(c));
  if (!leave_work)
    for (const char* f : {"CHERRY_PICK_HEAD", "MERGE_MSG", "MERGE_HEAD", "MERGE_MODE", "SQUASH_MSG", "AUTO_MERGE"}) c.remove_git_file(f);
  for (auto& [name, oid] : all_refs(c)) if (name.rfind("refs/rewritten/", 0) == 0) (void)c.repo->delete_ref(name);
}

// Everything done: the branch moves, hooks run, the state goes away.
void finish(Rb& rb) {
  Ctx& c = rb.c;
  const std::string head = head_oid(c);
  const std::string rewritten = rd(c, "rewritten-list").value_or("");
  if (rb.head_name.rfind("refs/", 0) == 0) {
    update_ref_checked(c, rb.head_name, head, rb.orig_head, reflog_msg(c, "finish", rb.head_name + " onto " + rb.onto));
    auto sr = c.repo->set_head(rb.head_name);
    if (!sr.ok()) die(c, sr.error().message);
    append_reflog(c, "HEAD", head, head, reflog_msg(c, "finish", "returning to " + rb.head_name));
  }
  std::vector<std::string> updated;
  std::vector<std::string> refs = lines_of(rd(c, "update-refs").value_or(""));
  for (size_t i = 0; i + 2 < refs.size(); i += 3) {
    const std::string &ref = refs[i], &old = refs[i + 1], &neu = refs[i + 2];
    if (neu == old || neu == std::string(40, '0')) continue;
    std::string cur = read_ref(c, ref).value_or("");
    if (cur != old && !(old == std::string(40, '0') && cur.empty())) {
      c.err << "warning: update of " << ref << " failed: it moved from " << old << " to " << cur << "\n";
      continue;
    }
    if (c.repo->update_ref(ref, neu).ok()) {
      append_reflog(c, ref, old, neu, reflog_msg(c, "finish", ref + " onto " + rb.onto));
      updated.push_back(ref);
    }
  }
  apply_autostash(rb);
  if (!rb.quiet) {
    clear_line(c);
    c.err << "Successfully rebased and updated " << (rb.head_name.rfind("refs/", 0) == 0 ? rb.head_name : std::string("detached HEAD")) << ".\n";
    if (!updated.empty()) {
      c.err << "Updated the following refs with --update-refs:\n";
      for (auto& u : updated) c.err << "\t" << u << "\n";
    }
  }
  remove_state(c);
  if (!rewritten.empty()) {
    HookOptions ho;
    ho.input = rewritten;
    run_hook(c, "post-rewrite", {"rebase"}, ho);
  }
}

// Runs what is left of the todo. Stops by throwing Exit with git's code, leaving the state as git leaves it.
int run_todo(Rb& rb, bool resumed) {
  Ctx& c = rb.c;
  // a rebase picked up again counts what is left; one just started has said how many there are
  std::string total_text = first_line(rd(c, "end"));
  if (resumed) {
    bool ok0 = true;
    size_t left = 0;
    for (auto& i : read_todo(c, ok0)) if (i.op != Op::Comment) ++left;
    total_text = std::to_string(done_lines(c).size() + left);
  }
  while (true) {
    bool ok = true;
    std::string error;
    std::vector<Item> todo = read_todo(c, ok, &error);
    if (!ok) {
      c.err << error
            << "You can fix this with 'git rebase --edit-todo' and then run 'git rebase --continue'.\nOr you can abort the rebase with 'git rebase --abort'.\n";
      return 1;
    }
    if (todo.empty()) break;
    Item item = todo.front();
    todo.erase(todo.begin());
    const std::string line = format_item(c, item, false, false);
    // the item moves from the todo to done before it runs
    write_todo(c, todo);
    app(c, "done", line);
    if (item.op == Op::Comment) continue;
    const size_t n = done_lines(c).size();
    wr(c, "msgnum", std::to_string(n) + "\n");
    if (!rb.quiet) {
      c.err << "Rebasing (" << n << "/" << total_text << ")" << (rb.verbose ? "\n" : "\r");
      c.err.flush();
    }
    try {
      switch (item.op) {
        case Op::Pick:
        case Op::Reword:
        case Op::Edit:
        case Op::Squash:
        case Op::Fixup: {
          do_pick(rb, item, (item.op == Op::Squash || item.op == Op::Fixup) && !next_is_fixup(todo, 0));
          if (item.op == Op::Edit) {
            GitCommit cm = read_commit(c, item.oid);
            stopped_for_amend(rb, cm, item.arg.empty() ? subject_of(cm.message) : item.arg);
          }
          record_in_rewritten(rb, item.oid, next_is_fixup(todo, 0));
          break;
        }
        case Op::Exec: {
          if (!do_exec(rb, item)) {
            c.err << "warning: execution failed: " << item.arg << "\nYou can fix the problem, and then run\n\n  git rebase --continue\n\n\n";
            if (rb.reschedule) {
              todo.insert(todo.begin(), item);
              write_todo(c, todo);
            }
            return 1;
          }
          if (!require_clean(c, "rebase", "")) {
            c.err << "warning: execution succeeded: " << item.arg
                  << "\nbut left changes to the index and/or the working tree\nCommit or stash your changes, and then run\n\n  git rebase --continue\n\n\n";
            return 1;
          }
          break;
        }
        case Op::Break: {
          std::string head = head_oid(c);
          clear_line(c);
          c.err << "Stopped at " << c.abbrev(head) << " (" << subject_of(read_commit(c, head).message) << ")\n";
          return 0;
        }
        case Op::Label: do_label(rb, item); break;
        case Op::Reset: do_reset(rb, item); break;
        case Op::Merge: do_merge(rb, item); break;
        case Op::UpdateRef: do_update_ref(rb, item); break;
        case Op::Noop:
        case Op::Drop:
        case Op::Comment: break;
      }
    } catch (const StopForError& e) {
      todo.insert(todo.begin(), item);
      write_todo(c, todo);
      std::string d = rd(c, "done").value_or("");
      if (d.size() >= line.size()) wr(c, "done", d.substr(0, d.size() - line.size()));
      wr(c, "msgnum", std::to_string(done_lines(c).size()) + "\n");
      return e.code;
    }
  }
  finish(rb);
  return 0;
}

// ---------------------------------------------------------------------------
// resuming
// ---------------------------------------------------------------------------
// What --continue does with the work left at a stop: the amend, the resolved conflict. 0 to go on.
int commit_staged_changes(Rb& rb, const std::vector<Item>& todo) {
  Ctx& c = rb.c;
  if (has_unstaged(c)) {
    c.err << "error: cannot rebase: You have unstaged changes.\n";
    return 1;
  }
  const bool clean = !has_staged(c);
  std::vector<std::string> done = done_lines(c);
  Item last;
  bool have_last = false;
  if (!done.empty()) {
    std::string perr;
    have_last = parse_line(c, done.back() + "\n", last, perr);
  }
  const bool final_fixup = !next_is_fixup(todo, 0);
  const std::string head = head_oid(c);
  if (has(c, "amend")) {
    std::string to_amend = first_line(rd(c, "amend"));
    if (!clean && head != to_amend) {
      c.err << "error: \nYou have uncommitted changes in your working tree. Please, commit them\nfirst and then run 'git rebase --continue' again.\n";
      return 1;
    }
    auto record_stopped = [&] {
      std::string sha = first_line(rd(c, "stopped-sha"));
      if (sha.size() == 40) record_in_rewritten(rb, sha, next_is_fixup(todo, 0));
    };
    const bool failed_fixup = have_last && (last.op == Op::Fixup || last.op == Op::Squash);
    if (clean && failed_fixup) clear_squash_state(c);   // nothing was staged: the chain starts over from HEAD
    if (!clean) {
      if (failed_fixup) {
        commit_fixup(rb, last, final_fixup, true);
        rm(c, "amend");
        c.remove_git_file("MERGE_MSG");
        record_stopped();
        return 0;
      }
      GitCommit hc = read_commit(c, head);
      std::string msg = rd(c, "message").value_or(hc.raw_message);
      msg = edit_message(rb, msg, edit_ctx_of(hc, parent_tree_of(c, hc)));
      if (msg.empty()) {
        c.err << "Aborting commit due to empty commit message.\n";
        return 1;
      }
      std::string tree = write_tree_of(c, c.repo->index());
      std::string a = author_of(hc);
      msg = through_prepare_hook(rb, msg);
      std::string oid = make_commit(c, tree, hc.parents, msg, a, committer_for(rb, a), true);
      move_head(c, oid, reflog_msg(c, "continue", first_line_of(msg)));
      if (!rb.quiet) print_new_commit(c, oid, true, hc.parents.empty());
      run_post_commit_hooks(c, false, "", oid);
    } else if (head == to_amend && have_last && last.op == Op::Reword) {
      reword_head(rb);
    }
    rm(c, "amend");
    c.remove_git_file("MERGE_MSG");
    record_stopped();
    return 0;
  }
  if (clean) {
    if (has(c, "stopped-sha")) {
      std::string sha = first_line(rd(c, "stopped-sha"));
      if (sha.size() == 40) record_in_rewritten(rb, sha, next_is_fixup(todo, 0));
    }
    return 0;
  }
  // a conflict that was resolved: its commit is made from the message kept for it
  if (!has(c, "message")) {
    c.err << "error: could not commit staged changes.\n";
    return 1;
  }
  std::string author = author_from_script_text(rd(c, "author-script").value_or(""));
  if (have_last && (last.op == Op::Fixup || last.op == Op::Squash)) {
    commit_fixup(rb, last, final_fixup);
  } else {
    GitCommit hc = read_commit(c, head);
    std::string who = author_from_script_text(rd(c, "author-script").value_or(""));
    size_t gt = who.find('>');
    std::string msg = edit_message(rb, rd(c, "message").value_or(""), EditCtx{gt == std::string::npos ? "" : who.substr(0, gt + 1), "", hc.tree, false});
    if (msg.empty()) {
      c.err << "Aborting commit due to empty commit message.\n";
      return 1;
    }
    std::string tree = write_tree_of(c, c.repo->index());
    std::string a = author.empty() ? c.ident(false) : author;
    msg = through_prepare_hook(rb, rb.signoff ? add_signoff(c, msg) : msg);
    std::string oid = make_commit(c, tree, {head}, msg, a, committer_for(rb, a), true);
    move_head(c, oid, reflog_msg(c, "continue", first_line_of(msg)));
    if (!rb.quiet) print_new_commit(c, oid, false, false);
    run_post_commit_hooks(c, false, "", oid);
  }
  std::string sha = first_line(rd(c, "stopped-sha"));
  if (sha.size() == 40) record_in_rewritten(rb, sha, next_is_fixup(todo, 0));
  c.remove_git_file("MERGE_MSG");
  c.remove_git_file("MERGE_HEAD");
  c.remove_git_file("AUTO_MERGE");
  return 0;
}

// ---------------------------------------------------------------------------
// the fork point: where the branch left an earlier incarnation of its upstream
// ---------------------------------------------------------------------------
std::string fork_point(Ctx& c, const std::string& upstream_name, const std::string& head) {
  std::string full = dwim_ref(c, upstream_name);
  if (full.empty()) return {};
  std::vector<std::string> tips;
  if (auto cur = read_ref(c, full)) tips.push_back(*cur);
  auto log = c.host().read_file(c.repo->common_dir() + "/logs/" + full);
  if (log.ok()) {
    std::vector<std::string> lines = lines_of(std::string((const char*)log->data(), log->size()));
    for (size_t i = lines.size(); i-- > 0;)
      if (lines[i].size() >= 82 && type_of(c, lines[i].substr(41, 40)) == "commit") tips.push_back(lines[i].substr(41, 40));
  }
  std::vector<std::string> uniq;
  for (auto& t : tips) if (std::find(uniq.begin(), uniq.end(), t) == uniq.end()) uniq.push_back(t);
  std::vector<std::string> common;
  for (auto& t : uniq)
    for (auto& b : merge_bases(c, head, t)) if (std::find(common.begin(), common.end(), b) == common.end()) common.push_back(b);
  std::vector<std::string> best;
  for (auto& x : common) {
    bool dominated = false;
    for (auto& y : common) if (x != y && is_ancestor(c, x, y)) dominated = true;
    if (!dominated) best.push_back(x);
  }
  std::stable_sort(best.begin(), best.end(), [&](const std::string& a, const std::string& b) {
    return read_commit(c, a).committer_time > read_commit(c, b).committer_time;
  });
  for (auto& b : best) if (std::find(uniq.begin(), uniq.end(), b) != uniq.end()) return b;
  return {};
}

// ---------------------------------------------------------------------------
// starting
// ---------------------------------------------------------------------------

void print_edit_advice(Ctx& c) {
  c.err << "You can fix this with 'git rebase --edit-todo' and then run 'git rebase --continue'.\n"
           "Or you can abort the rebase with 'git rebase --abort'.\n";
}

// What git warns of when an edited todo no longer names some of the commits it started with (newest first).
std::string dropped_commits(Ctx& c, const std::vector<Item>& original, const std::vector<Item>& edited) {
  std::set<std::string> kept;
  for (auto& i : edited) if (!i.oid.empty()) kept.insert(i.oid);
  std::string missing;
  for (size_t i = original.size(); i-- > 0;) {
    if (original[i].op == Op::Comment || original[i].oid.empty() || kept.count(original[i].oid)) continue;
    missing += " - " + c.abbrev(original[i].oid) + " " + original[i].arg + "\n";
  }
  if (missing.empty()) return {};
  return "Warning: some commits may have been dropped accidentally.\nDropped commits (newer to older):\n" + missing +
         "To avoid this message, use \"drop\" to explicitly remove a commit.\n\n"
         "Use 'git config rebase.missingCommitsCheck' to change the level of warnings.\n"
         "The possible behaviours are: ignore, warn, error.\n\n";
}

// Switches to the branch (or commit) a rebase was asked to rebase, as `git checkout` does, quietly.
void switch_to(Ctx& c, const std::string& name) {
  const std::string full = "refs/heads/" + name;
  const bool is_branch = c.repo->ref_oid(full).ok();
  std::string target = is_branch ? *c.repo->ref_oid(full) : commit_of(c, name);
  const std::string before = head_oid(c);
  std::string why;
  if (!checkout_commit(c, target, &why)) {
    c.err << "error: " << why << "\nAborting\n";
    throw Exit{1};
  }
  if (is_branch) {
    auto r = c.repo->set_head(full);
    if (!r.ok()) die(c, r.error().message);
  } else {
    auto r = c.repo->set_head_detached(target);
    if (!r.ok()) die(c, r.error().message);
  }
  append_reflog(c, "HEAD", before, target, "rebase: checkout " + name);
}

// The leading picks that already sit on the base need no replay: they go to `done`.
size_t skip_unnecessary_picks(Rb& rb, std::vector<Item>& items, std::string& base) {
  Ctx& c = rb.c;
  size_t i = 0;
  for (; i < items.size(); ++i) {
    const Op op = items[i].op;
    if (op == Op::Noop || op == Op::Drop || op == Op::Comment) continue;
    if (op != Op::Pick) break;
    GitCommit cm = read_commit(c, items[i].oid);
    if ((cm.parents.empty() ? std::string() : cm.parents[0]) != base) break;
    base = cm.oid;
  }
  if (i == 0) return 0;
  wr(c, "done", format_todo(c, items, false, false, i));
  items.erase(items.begin(), items.begin() + (long)i);
  if (next_is_fixup(items, 0)) record_in_rewritten(rb, base, true);
  return i;
}

void abort_rebase(Rb& rb);

struct StartOptions {
  bool interactive = false, root = false, keep_base = false, force = false, autosquash = false, autostash = false;
  bool rebase_merges = false, cousins = false, update_refs = false, keep_empty = true, reapply = false, verify = true;
  bool quiet = false, verbose = false;
  bool explicit_upstream = false;
  std::string empty_mode, strategy, strategy_opt, onto_arg;
  std::vector<std::string> exec;
};

int start_rebase(Ctx& c, const Parsed& p, const StartOptions& o) {
  std::vector<std::string> pos = p.pos;
  std::string upstream_name, branch_arg;
  if (o.root) {
    if (pos.size() > 1) usage(c, "usage: git rebase [options] --root [<branch>]");
    if (!pos.empty()) branch_arg = pos[0];
  } else {
    if (pos.size() > 2) usage(c, "usage: git rebase [options] [<upstream> [<branch>]]");
    if (!pos.empty()) upstream_name = pos[0];
    if (pos.size() > 1) branch_arg = pos[1];
  }
  if (!read_ref(c, "HEAD")) die(c, "Needed a single revision");
  std::string cur_branch = current_branch(c);
  bool configured_upstream = false;
  if (upstream_name.empty() && !o.root) {
    std::string b = branch_arg.empty() ? cur_branch : branch_arg;
    if (b.empty()) {
      c.err << "You are not currently on a branch.\nPlease specify which branch you want to rebase against.\n"
               "See git-rebase(1) for details.\n\n    git rebase '<branch>'\n\n";
      return 1;
    }
    std::string up = upstream_of(c, b);
    if (up.empty()) {
      c.err << "There is no tracking information for the current branch.\nPlease specify which branch you want to rebase against.\n"
               "See git-rebase(1) for details.\n\n    git rebase '<branch>'\n\n"
               "If you wish to set tracking information for this branch you can do so with:\n\n"
               "    git branch --set-upstream-to=<remote>/<branch> " << b << "\n\n";
      return 1;
    }
    upstream_name = up;
    configured_upstream = true;
  }
  std::string upstream;
  if (!o.root) {
    auto u = resolve(c, upstream_name + "^{commit}");
    if (!u) die(c, "invalid upstream '" + upstream_name + "'");
    upstream = *u;
  }
  // the branch to rebase is checked out first
  if (!branch_arg.empty() && branch_arg != cur_branch) {
    if (!resolve(c, branch_arg + "^{commit}")) die(c, "no such branch/commit '" + branch_arg + "'");
    switch_to(c, branch_arg);
    cur_branch = current_branch(c);
  }
  const std::string orig_head = head_oid(c);
  if (orig_head.empty()) die(c, "Needed a single revision");
  // the hook may say no
  if (o.verify) {
    std::vector<std::string> hargs;
    if (!o.root) hargs.push_back(upstream_name);
    if (!branch_arg.empty()) hargs.push_back(branch_arg);
    HookResult hr = run_hook(c, "pre-rebase", hargs);
    if (hr.ran && hr.code != 0) die(c, "The pre-rebase hook refused to rebase.");
  }
  std::string onto, onto_name = upstream_name;
  if (!o.onto_arg.empty()) {
    onto_name = o.onto_arg;
    size_t dots = o.onto_arg.find("...");
    if (dots != std::string::npos) {
      std::string l = o.onto_arg.substr(0, dots), r = o.onto_arg.substr(dots + 3);
      if (l.empty()) l = "HEAD";
      if (r.empty()) r = "HEAD";
      auto lc = resolve(c, l + "^{commit}"), rc = resolve(c, r + "^{commit}");
      if (!lc || !rc) die(c, "Does not point to a valid commit '" + o.onto_arg + "'");
      auto b = merge_bases(c, *lc, *rc);
      if (b.empty()) die(c, "'" + o.onto_arg + "': there is no merge base");
      if (b.size() > 1) die(c, "'" + o.onto_arg + "': there are more than one merge bases");
      onto = b[0];
    } else {
      auto r = resolve(c, o.onto_arg + "^{commit}");
      if (!r) die(c, "Does not point to a valid commit '" + o.onto_arg + "'");
      onto = *r;
    }
  } else if (o.keep_base) {
    auto b = merge_bases(c, upstream, orig_head);
    if (b.empty()) die(c, "No merge base between '" + upstream_name + "' and the current branch");
    if (b.size() > 1) die(c, "'" + upstream_name + "': there are more than one merge bases");
    onto = b[0];
  } else if (!o.root) {
    onto = upstream;
  }
  Rb rb(c);
  rb.quiet = o.quiet;
  rb.verbose = o.verbose;
  rb.allow_ff = !o.force;
  rb.signoff = p.has("signoff");
  rb.strategy_opt = o.strategy_opt;
  rb.cdate_is_adate = p.has("committer-date-is-author-date") || c.config.get_bool("rebase.committerdateisauthordate", false);
  rb.ignore_date = p.has("ignore-date") || p.has("reset-author-date");
  rb.drop_redundant = o.empty_mode == "drop";
  rb.keep_redundant = o.empty_mode == "keep";
  rb.head_name = cur_branch.empty() ? "detached HEAD" : "refs/heads/" + cur_branch;
  rb.orig_head = orig_head;
  // the fork point (of the upstream's earlier incarnations) bounds what is replayed
  std::string left_of = o.root ? onto : upstream;
  if (!o.root && !o.keep_base && (configured_upstream || p.has("fork-point")) && !p.negated.count("fork-point")) {
    std::string fp = fork_point(c, upstream_name, orig_head);
    if (!fp.empty()) left_of = fp;
  }
  const bool plain = !o.interactive && o.exec.empty() && !o.rebase_merges && !o.update_refs;
  // dirty trees stop here unless they are stashed
  std::string autostash;
  if (o.autostash) {
    if (has_unstaged(c) || has_staged(c)) {
      autostash = create_autostash(c);
      if (!autostash.empty()) {
        c.out << "Created autostash: " << c.abbrev(autostash) << "\n";
        hard_reset_to(c, orig_head);
      }
    }
  } else if (!require_clean(c, "rebase", "Please commit or stash them.")) {
    return 1;
  }
  const std::string branch_name = cur_branch.empty() ? "HEAD" : cur_branch;
  if (!o.root && plain) {
    auto bases = merge_bases(c, upstream, orig_head);
    const bool can_ff = bases.size() == 1 && bases[0] == onto && (left_of == upstream || left_of == bases[0]);
    if (can_ff) {
      if (!o.force) {
        if (!o.quiet) c.out << "Current branch " << branch_name << " is up to date.\n";
        if (!autostash.empty()) {
          if (apply_stash_commit(c, autostash)) c.err << "Applied autostash.\n";
          else {
            store_stash(c, autostash);
            c.err << "Applying autostash resulted in conflicts.\nYour changes are safe in the stash.\nYou can run \"git stash pop\" or \"git stash drop\" at any time.\n";
          }
        }
        return 0;
      }
      if (!o.quiet) c.out << "Current branch " << branch_name << " is up to date, rebase forced.\n";
    }
  }
  // the state directory
  (void)c.host().make_dir(sdir(c));
  wr(c, "head-name", rb.head_name + "\n");
  wr(c, "interactive", "");
  if (o.quiet) wr(c, "quiet", "");
  if (o.verbose) wr(c, "verbose", "");
  if (o.empty_mode == "drop") wr(c, "drop_redundant_commits", "");
  if (o.empty_mode == "keep") wr(c, "keep_redundant_commits", "");
  wr(c, o.exec.empty() && c.config.get_bool("rebase.reschedulefailedexec", false) ? "reschedule-failed-exec" : "no-reschedule-failed-exec", "");
  rb.reschedule = c.config.get_bool("rebase.reschedulefailedexec", false);
  if (rb.signoff) wr(c, "signoff", "--signoff\n");
  if (!o.strategy.empty()) wr(c, "strategy", o.strategy + "\n");
  if (!o.strategy_opt.empty()) wr(c, "strategy_opts", " --" + o.strategy_opt + "\n");
  if (c.sign == 1) wr(c, "gpg_sign_opt", "-S" + c.sign_key + "\n");
  if (!autostash.empty()) wr(c, "autostash", autostash + "\n");
  // --root without --onto replays everything on a new empty root commit
  if (o.root && onto.empty()) {
    std::string squash_onto = make_commit(c, empty_tree_oid(), {}, "", {}, {}, false);
    wr(c, "squash-onto", squash_onto + "\n");
    rb.squash_onto = squash_onto;
    onto = squash_onto;
    onto_name = squash_onto;
  }
  rb.onto = onto;
  wr(c, "onto", onto + "\n");
  wr(c, "orig-head", orig_head + "\n");
  // the todo
  ScriptOptions so;
  so.keep_empty = o.keep_empty;
  so.rebase_merges = o.rebase_merges;
  so.rebase_cousins = o.cousins;
  so.reapply_cherry_picks = o.reapply;
  so.update_refs = o.update_refs;
  so.letters = letters_on(c);
  so.onto = onto;
  so.head_name = rb.head_name;
  std::vector<Item> items = make_script(c, left_of, orig_head, so);
  if (o.autosquash) rearrange_squash(c, items);
  if (o.update_refs) add_update_ref_commands(c, items, rb.head_name);
  add_exec_commands(items, o.exec);
  if (count_commands(items) == 0) {
    Item n;
    n.op = Op::Noop;
    items.push_back(n);
  }
  const std::string short_range = o.root ? c.abbrev(orig_head) : c.abbrev(left_of) + ".." + c.abbrev(orig_head);
  const std::string help = todo_help(c, short_range, c.abbrev(onto), count_commands(items), false);
  wr(c, "git-rebase-todo", format_todo(c, items, false, so.letters) + "\n" + help);
  wr(c, "git-rebase-todo.backup", format_todo(c, items, false, so.letters) + "\n" + help);
  const std::string check = c.config.get("rebase.missingcommitscheck").value_or("ignore");
  if (o.interactive) {
    wr(c, "git-rebase-todo", format_todo(c, items, true, so.letters) + "\n" + help);
    try {
      edit_file(c, spath(c, "git-rebase-todo"), true, "the editor");
    } catch (const Exit& e) {
      // an editor that fails ends the rebase before anything has moved
      remove_state(c);
      if (!autostash.empty()) {
        rb.head_name.clear();
        if (apply_stash_commit(c, autostash)) c.err << "Applied autostash.\n";
      }
      return e.code;
    }
    reopen(c);
  }
  std::vector<Item> edited;
  std::string error;
  std::string text = strip_todo_space(c, rd(c, "git-rebase-todo").value_or(""));
  bool parsed = parse_todo(c, text, edited, error);
  auto back_to_onto = [&] {
    std::string why;
    if (checkout_commit(c, onto, &why)) detach_head(c, onto, reflog_msg(c, "start", "checkout " + onto_name));
    c.write_git_file("ORIG_HEAD", orig_head + "\n");
  };
  if (!parsed) {
    c.err << error;
    print_edit_advice(c);
    back_to_onto();
    return 1;
  }
  if (count_commands(edited) == 0) {
    c.err << "error: nothing to do\n";
    remove_state(c);
    if (!autostash.empty()) {
      rb.head_name.clear();
      if (apply_stash_commit(c, autostash)) c.err << "Applied autostash.\n";
    }
    return 1;
  }
  if (check != "ignore") {
    std::string missing = dropped_commits(c, items, edited);
    if (!missing.empty()) {
      c.err << missing;
      print_edit_advice(c);
      if (check == "error") {
        wr(c, "dropped", "");
        back_to_onto();
        return 1;
      }
    }
  }
  const size_t total = edited.size();
  std::string base = onto;
  if (rb.allow_ff) skip_unnecessary_picks(rb, edited, base);
  wr(c, "git-rebase-todo", format_todo(c, edited, false, so.letters));
  wr(c, "end", std::to_string(total) + "\n");
  {
    // every ref an update-ref line will move is noted with the value it has now
    std::string refs_state;
    for (auto& it : edited) {
      if (it.op != Op::UpdateRef) continue;
      refs_state += it.arg + "\n" + read_ref(c, it.arg).value_or(std::string(40, '0')) + "\n" + std::string(40, '0') + "\n";
    }
    if (!refs_state.empty()) wr(c, "update-refs", refs_state);
  }
  {
    std::string why;
    if (!checkout_commit(c, base, &why)) {
      c.err << "error: " << why << "\nAborting\nfatal: Could not detach HEAD\n";
      remove_state(c);
      return 128;
    }
    detach_head(c, base, reflog_msg(c, "start", "checkout " + onto_name));
    c.write_git_file("ORIG_HEAD", orig_head + "\n");
    run_post_checkout_hook(c, orig_head, base, true);
  }
  return run_todo(rb, false);
}

// ---------------------------------------------------------------------------
// the actions on a rebase in progress
// ---------------------------------------------------------------------------
int action_continue(Ctx& c) {
  Rb rb(c);
  load_opts(rb);
  if (!c.repo->unmerged().empty()) {
    std::set<std::string> seen;
    for (auto& u : c.repo->unmerged()) if (seen.insert(u.path).second) c.out << u.path << ": needs merge\n";
    c.out << "You must edit all merge conflicts and then\nmark them as resolved using git add\n";
    return 1;
  }
  bool ok = true;
  std::string error;
  std::vector<Item> todo = read_todo(c, ok, &error);
  if (!ok) {
    c.err << error;
    print_edit_advice(c);
    return 1;
  }
  try {
    int rc = commit_staged_changes(rb, todo);
    if (rc) return rc;
    for (const char* f : {"message", "author-script", "stopped-sha"}) rm(c, f);
    // a commit that amended (squash, fixup) leaves REBASE_HEAD for the next command to move
    std::vector<std::string> done = done_lines(c);
    const bool amended = !done.empty() && (done.back().rfind("squash", 0) == 0 || done.back().rfind("fixup", 0) == 0);
    if (!amended) c.remove_git_file("REBASE_HEAD");
  } catch (const StopForError& e) {
    return e.code;
  }
  return run_todo(rb, true);
}

int action_skip(Ctx& c) {
  Rb rb(c);
  load_opts(rb);
  hard_reset_to(c, head_oid(c));
  for (const char* f : {"MERGE_MSG", "MERGE_HEAD", "MERGE_MODE", "AUTO_MERGE", "REBASE_HEAD"}) c.remove_git_file(f);
  rm(c, "amend");
  return run_todo(rb, true);
}

void abort_rebase(Rb& rb) {
  Ctx& c = rb.c;
  const std::string before = head_oid(c);
  hard_reset_to(c, rb.orig_head);
  if (rb.head_name.rfind("refs/", 0) == 0) {
    auto sr = c.repo->set_head(rb.head_name);
    if (!sr.ok()) die(c, sr.error().message);
    append_reflog(c, "HEAD", before, rb.orig_head, reflog_msg(c, "abort", "returning to " + rb.head_name));
  } else {
    detach_head(c, rb.orig_head, reflog_msg(c, "abort", "returning to " + rb.orig_head));
  }
  c.write_git_file("ORIG_HEAD", rb.orig_head + "\n");
  apply_autostash(rb);
  remove_state(c);
  c.remove_git_file("REBASE_HEAD");
}

int action_abort(Ctx& c) {
  Rb rb(c);
  load_opts(rb);
  abort_rebase(rb);
  return 0;
}

int action_quit(Ctx& c) {
  Rb rb(c);
  load_opts(rb);
  std::string stash = first_line(rd(c, "autostash"));
  remove_state(c, true);
  if (stash.size() == 40) {
    store_stash(c, stash);
    c.err << "Your changes are safe in the stash.\nYou can run \"git stash pop\" or \"git stash drop\" at any time.\n";
  }
  return 0;
}

int action_edit_todo(Ctx& c) {
  Rb rb(c);
  load_opts(rb);
  bool ok = true;
  std::string error;
  std::vector<Item> todo = read_todo(c, ok, nullptr);
  if (!ok) todo.clear();
  const bool letters_ = letters_on(c);
  const std::string help = todo_help(c, "", "", 0, true);
  wr(c, "git-rebase-todo.backup", format_todo(c, todo, false, letters_) + help);
  wr(c, "git-rebase-todo", format_todo(c, todo, true, letters_) + help);
  edit_file(c, spath(c, "git-rebase-todo"), true, "the editor");
  reopen(c);
  std::vector<Item> edited;
  if (!parse_todo(c, strip_todo_space(c, rd(c, "git-rebase-todo").value_or("")), edited, error, has(c, "done"))) {
    c.err << error;
    print_edit_advice(c);
    return 1;
  }
  write_todo(c, edited);
  return 0;
}

}  // namespace

bool rebase_in_progress(Ctx& c) { return c.repo && in_progress(c); }

std::string rebase_status_text(Ctx& c, bool in_template) { return status_text(c, in_template, false); }

namespace {
std::string status_text(Ctx& c, bool in_template, bool as_conflict) {
  if (!c.repo || !in_progress(c)) return {};
  const bool interactive = has(c, "interactive");
  std::string branch = first_line(rd(c, "head-name"));
  if (branch.rfind("refs/heads/", 0) == 0) branch = branch.substr(11);
  std::string onto = first_line(rd(c, "onto"));
  const std::string onto_short = onto.size() == 40 ? c.abbrev(onto) : onto;
  std::string out = std::string(interactive ? "interactive rebase" : "rebase") + " in progress; onto " + onto_short + "\n";
  auto show = [&](const std::string& l) {
    // a todo line shows its commit, the second word, abbreviated (when there is a third)
    size_t a = l.find(' ');
    size_t b = a == std::string::npos ? a : l.find(' ', a + 1);
    if (b == std::string::npos) return l;
    const std::string cmd = l.substr(0, a), word = l.substr(a + 1, b - a - 1);
    if (cmd == "exec" || cmd == "x" || cmd == "label" || cmd == "l" || cmd == "reset" || cmd == "t" || cmd == "update-ref" || cmd == "u") return l;
    if (word.size() != 40 || !std::all_of(word.begin(), word.end(), [](char ch) { return std::isxdigit((unsigned char)ch); })) return l;
    return cmd + " " + c.abbrev(word) + l.substr(b);
  };
  if (interactive) {
    std::vector<std::string> done = done_lines(c), todo;
    for (auto& l : lines_of(rd(c, "git-rebase-todo").value_or(""))) if (!l.empty() && l[0] != comment_char(c)) todo.push_back(l);
    if (!has(c, "git-rebase-todo")) out += "git-rebase-todo is missing.\n";
    if (done.empty()) out += "No commands done.\n";
    else {
      out += done.size() == 1 ? "Last command done (1 command done):\n" : "Last commands done (" + std::to_string(done.size()) + " commands done):\n";
      for (size_t i = done.size() > 2 ? done.size() - 2 : 0; i < done.size(); ++i) out += "   " + show(done[i]) + "\n";
      if (done.size() > 2 && !in_template) out += "  (see more in file " + spath(c, "done") + ")\n";
    }
    if (todo.empty()) out += "No commands remaining.\n";
    else {
      out += todo.size() == 1 ? "Next command to do (1 remaining command):\n" : "Next commands to do (" + std::to_string(todo.size()) + " remaining commands):\n";
      for (size_t i = 0; i < 2 && i < todo.size(); ++i) out += "   " + show(todo[i]) + "\n";
      if (!in_template) out += "  (use \"git rebase --edit-todo\" to view and edit)\n";
    }
  }
  if (!c.repo->unmerged().empty()) {
    out += "You are currently rebasing branch '" + branch + "' on '" + onto_short + "'.\n";
    if (!in_template)
      out += "  (fix conflicts and then run \"git rebase --continue\")\n  (use \"git rebase --skip\" to skip this patch)\n"
             "  (use \"git rebase --abort\" to check out the original branch)\n";
  } else if (!interactive || as_conflict || c.read_git_file("MERGE_MSG")) {
    out += "You are currently rebasing branch '" + branch + "' on '" + onto_short + "'.\n";
    if (!in_template) out += "  (all conflicts fixed: run \"git rebase --continue\")\n";
  } else {
    out += "You are currently editing a commit while rebasing branch '" + branch + "' on '" + onto_short + "'.\n";
    if (!in_template)
      out += "  (use \"git commit --amend\" to amend the current commit)\n  (use \"git rebase --continue\" once you are satisfied with your changes)\n";
  }
  return out + "\n";
}
}  // namespace

int cmd_rebase(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'i', "interactive"}, {'r', "rebase-merges", Arg::Optional}, {0, "onto", Arg::Required}, {0, "keep-base"},
                                         {0, "root"}, {'x', "exec", Arg::Required}, {'f', "force-rebase"}, {0, "ff"}, {0, "autosquash"},
                                         {0, "autostash"}, {0, "continue"}, {0, "abort"}, {0, "skip"}, {0, "quit"}, {0, "edit-todo"},
                                         {0, "show-current-patch", Arg::Optional}, {0, "keep-empty"}, {0, "empty", Arg::Required},
                                         {'q', "quiet"}, {'v', "verbose"}, {0, "stat"}, {'n', "no-stat"}, {0, "signoff"}, {'C', "C", Arg::Required},
                                         {0, "whitespace", Arg::Required}, {0, "ignore-whitespace"}, {0, "committer-date-is-author-date"},
                                         {0, "ignore-date"}, {0, "reset-author-date"}, {'S', "gpg-sign", Arg::Optional},
                                         {'s', "strategy", Arg::Required}, {'X', "strategy-option", Arg::Required}, {0, "rerere-autoupdate"},
                                         {0, "fork-point"}, {0, "update-refs"}, {0, "reapply-cherry-picks"}, {0, "allow-empty-message"},
                                         {0, "reschedule-failed-exec"}, {0, "apply"}, {'m', "merge"}, {0, "verify"}, {0, "no-verify"},
                                         {0, "progress"}});
  c.open_repo();
  take_sign_option(c, p);
  if (p.has("apply") || p.has("C") || p.has("whitespace") || p.has("ignore-whitespace") || p.has("rerere-autoupdate"))
    unsupported(c, "rebase --apply/-C/--whitespace/--ignore-whitespace/--rerere-autoupdate (the merge backend is the only one)");
  if (p.has("allow-empty-message")) unsupported(c, "rebase --allow-empty-message");
  int actions = 0;
  for (const char* a : {"continue", "abort", "skip", "quit", "edit-todo", "show-current-patch"}) if (p.has(a)) ++actions;
  if (actions > 1) usage(c, "options '--continue', '--abort', '--skip', '--quit', '--edit-todo' and '--show-current-patch' cannot be used together");
  if (actions == 1) {
    if (!in_progress(c)) die(c, "No rebase in progress?");
    if (p.has("show-current-patch")) {
      if (!c.read_git_file("REBASE_HEAD")) die(c, "No rebase in progress?");
      c.args = {"show", "REBASE_HEAD", "--"};
      if (p.get("show-current-patch") == "raw") c.args = {"show", "--format=raw", "-s", "REBASE_HEAD", "--"};
      else if (!p.get("show-current-patch").empty() && p.get("show-current-patch") != "diff") usage(c, "unknown mode for --show-current-patch: " + p.get("show-current-patch"));
      return cmd_show(c);
    }
    try {
      if (p.has("continue")) return action_continue(c);
      if (p.has("skip")) return action_skip(c);
      if (p.has("abort")) return action_abort(c);
      if (p.has("quit")) return action_quit(c);
      return action_edit_todo(c);
    } catch (const StopForError& e) {
      return e.code;
    }
  }
  if (in_progress(c)) {
    c.err << "fatal: It seems that there is already a rebase-merge directory, and\n"
             "I wonder if you are in the middle of another rebase.  If that is the\n"
             "case, please try\n\tgit rebase (--continue | --abort | --skip)\n"
             "If that is not the case, please\n\trm -fr \"" << (c.repo->git_dir() == c.top + "/.git" && c.cwd == c.top ? std::string(".git/rebase-merge") : sdir(c)) << "\"\n"
             "and run me again.  I am stopping in case you still have something\nvaluable there.\n\n";
    return 128;
  }
  StartOptions o;
  o.interactive = p.has("interactive");
  o.root = p.has("root");
  o.keep_base = p.has("keep-base");
  o.force = p.has("force-rebase") || p.negated.count("ff") > 0;
  o.quiet = p.has("quiet");
  o.verbose = p.has("verbose");
  o.onto_arg = p.get("onto");
  o.exec = p.all("exec");
  o.verify = !p.negated.count("verify") && !p.has("no-verify");
  o.keep_empty = !p.negated.count("keep-empty");
  o.reapply = p.has("reapply-cherry-picks");
  o.rebase_merges = p.has("rebase-merges");
  o.cousins = p.get("rebase-merges") == "rebase-cousins";
  if (o.rebase_merges && !p.get("rebase-merges").empty() && p.get("rebase-merges") != "rebase-cousins" && p.get("rebase-merges") != "no-rebase-cousins")
    usage(c, "unknown mode: " + p.get("rebase-merges"));
  o.update_refs = p.has("update-refs") || (!p.negated.count("update-refs") && c.config.get_bool("rebase.updaterefs", false));
  o.autostash = p.has("autostash") || (!p.negated.count("autostash") && c.config.get_bool("rebase.autostash", false));
  o.autosquash = o.interactive && (p.has("autosquash") || (!p.negated.count("autosquash") && c.config.get_bool("rebase.autosquash", false)));
  o.strategy = p.get("strategy");
  if (!o.strategy.empty() && o.strategy != "ort" && o.strategy != "recursive") unsupported(c, "the merge strategy '" + o.strategy + "'");
  o.strategy_opt = p.get("strategy-option");
  std::string em = p.get("empty");
  if (!em.empty() && em != "drop" && em != "keep" && em != "ask" && em != "stop") usage(c, "unrecognized empty type '" + em + "'; valid values are \"drop\", \"keep\", and \"ask\".");
  o.empty_mode = em == "stop" ? "ask" : em.empty() ? (o.interactive ? "ask" : !o.exec.empty() ? "keep" : "drop") : em;
  if (o.root && o.keep_base) usage(c, "options '--keep-base' and '--root' cannot be used together");
  if (o.keep_base && !o.onto_arg.empty()) usage(c, "options '--keep-base' and '--onto' cannot be used together");
  try {
    return start_rebase(c, p, o);
  } catch (const StopForError& e) {
    return e.code;
  }
}

int rebase_for_pull(Ctx& c, const std::vector<std::string>& args, const std::string& reflog_action) {
  c.args = {"rebase"};
  c.args.insert(c.args.end(), args.begin(), args.end());
  if (!c.hasenv("GIT_REFLOG_ACTION")) c.env["GIT_REFLOG_ACTION"] = reflog_action;
  return cmd_rebase(c);
}

std::string rebase_fork_point(Ctx& c, const std::string& upstream_name, const std::string& head) { return fork_point(c, upstream_name, head); }

bool rebase_require_clean(Ctx& c, const std::string& action, const std::string& hint) { return require_clean(c, action, hint); }

}  // namespace aios_git
