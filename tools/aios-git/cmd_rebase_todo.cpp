// The rebase todo list: parsing and writing its lines as git does, and the list `git rebase` starts from.
#include "rebase_todo.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>

namespace aios_git::rebase {

using namespace aios;
using namespace aios::userland;

namespace {

struct OpName { Op op; const char* name; char letter; };
const OpName kOps[] = {{Op::Pick, "pick", 'p'},         {Op::Edit, "edit", 'e'},   {Op::Reword, "reword", 'r'},
                       {Op::Fixup, "fixup", 'f'},       {Op::Squash, "squash", 's'}, {Op::Exec, "exec", 'x'},
                       {Op::Break, "break", 'b'},       {Op::Label, "label", 'l'}, {Op::Reset, "reset", 't'},
                       {Op::Merge, "merge", 'm'},       {Op::UpdateRef, "update-ref", 'u'}, {Op::Noop, "noop", 0},
                       {Op::Drop, "drop", 'd'}};

std::string trim_right(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
  return s;
}

size_t span_blank(const std::string& s, size_t at) {
  while (at < s.size() && (s[at] == ' ' || s[at] == '\t')) ++at;
  return at;
}

size_t word_end(const std::string& s, size_t at) {
  while (at < s.size() && s[at] != ' ' && s[at] != '\t' && s[at] != '\n' && s[at] != '\r') ++at;
  return at;
}

// The subject git's todo shows for a commit (rebase.instructionFormat, "%s" by default).
std::string todo_subject(Ctx& c, const GitCommit& cm) {
  std::string fmt = c.config.get("rebase.instructionformat").value_or("%s");
  if (fmt == "%s") return subject_of(cm.message);
  std::string out;
  for (size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '%' || i + 1 >= fmt.size()) { out += fmt[i]; continue; }
    char k = fmt[++i];
    if (k == '%') out += '%';
    else if (k == 's') out += subject_of(cm.message);
    else if (k == 'h') out += c.abbrev(cm.oid);
    else if (k == 'H') out += cm.oid;
    else if (k == 'a' && i + 1 < fmt.size() && fmt[i + 1] == 'n') { out += cm.author.substr(0, cm.author.find(" <")); ++i; }
    else if (k == 'a' && i + 1 < fmt.size() && fmt[i + 1] == 'e') {
      size_t lt = cm.author.find('<'), gt = cm.author.find('>');
      if (lt != std::string::npos && gt != std::string::npos) out += cm.author.substr(lt + 1, gt - lt - 1);
      ++i;
    } else unsupported(c, "rebase.instructionFormat with %" + std::string(1, k));
  }
  return out;
}

bool is_original_empty(Ctx& c, const GitCommit& cm) {
  if (cm.parents.empty()) return cm.tree == empty_tree_oid();
  return read_commit(c, cm.parents[0]).tree == cm.tree;
}

std::string whitespace_free(const std::string& s) {
  std::string o;
  for (char ch : s) if (!std::isspace((unsigned char)ch)) o += ch;
  return o;
}

}  // namespace

const char* op_name(Op op) {
  for (auto& o : kOps) if (o.op == op) return o.name;
  return "";
}
char op_letter(Op op) {
  for (auto& o : kOps) if (o.op == op) return o.letter;
  return 0;
}

char comment_char(Ctx& c) {
  auto v = c.config.get("core.commentchar");
  if (v && v->size() == 1 && (*v)[0] != 'a') return (*v)[0];
  return '#';
}

bool parse_line(Ctx& c, const std::string& line_in, Item& out, std::string& error) {
  out = Item{};
  const std::string& line = line_in;
  size_t at = span_blank(line, 0);
  if (at >= line.size() || line[at] == '\n' || line[at] == '\r' || line[at] == comment_char(c)) {
    out.op = Op::Comment;
    out.text = trim_right(line);
    return true;
  }
  size_t we = word_end(line, at);
  std::string word = line.substr(at, we - at);
  const OpName* found = nullptr;
  for (auto& o : kOps) if (word == o.name || (o.letter && word.size() == 1 && word[0] == o.letter)) found = &o;
  if (!found) { error.clear(); return false; }
  out.op = found->op;
  size_t rest = span_blank(line, we);
  std::string tail = line.substr(rest);
  std::string tail_trim = trim_right(tail);
  if (out.op == Op::Noop || out.op == Op::Break) {
    if (!tail_trim.empty()) { error = std::string(found->name) + " does not accept arguments: '" + tail + "'"; return false; }
    return true;
  }
  if (out.op == Op::Exec) {
    if (tail_trim.empty()) { error = "missing arguments for exec"; return false; }
    out.arg = tail_trim;
    return true;
  }
  if (out.op == Op::Label || out.op == Op::Reset || out.op == Op::UpdateRef) {
    if (tail_trim.empty()) { error = std::string("missing arguments for ") + found->name; return false; }
    out.arg = tail_trim;
    if (out.op == Op::UpdateRef) {
      if (out.arg.rfind("refs/", 0) != 0 && out.arg != "HEAD") { error = "'" + out.arg + "' is not a valid ref name"; return false; }
    }
    return true;
  }
  if (out.op == Op::Merge) {
    size_t p = 0;
    if (tail_trim.size() > 2 && tail_trim[0] == '-' && (tail_trim[1] == 'C' || tail_trim[1] == 'c')) {
      out.flag = tail_trim[1];
      p = span_blank(tail_trim, 2);
      size_t e = word_end(tail_trim, p);
      std::string name = tail_trim.substr(p, e - p);
      if (name.empty()) { error = "missing arguments for merge"; return false; }
      auto r = resolve(c, name + "^{commit}");
      if (!r) { error = "could not parse '" + name + "'"; return false; }
      out.oid = *r;
      p = span_blank(tail_trim, e);
    }
    out.arg = tail_trim.substr(p);
    if (out.arg.empty()) { error = "missing arguments for merge"; return false; }
    return true;
  }
  // the commit-taking commands
  if (out.op == Op::Fixup && tail_trim.size() > 2 && tail_trim[0] == '-' && (tail_trim[1] == 'C' || tail_trim[1] == 'c') &&
      (tail_trim[2] == ' ' || tail_trim[2] == '\t')) {
    out.flag = tail_trim[1];
    rest = span_blank(tail, 2);
    tail = tail.substr(rest);
    tail_trim = trim_right(tail);
  }
  if (tail_trim.empty()) { error = std::string("missing arguments for ") + found->name; return false; }
  size_t e = word_end(tail_trim, 0);
  std::string name = tail_trim.substr(0, e);
  auto r = resolve(c, name + "^{commit}");
  if (!r) { error = "could not parse '" + name + "'"; return false; }
  out.oid = *r;
  out.arg = tail_trim.substr(span_blank(tail_trim, e));
  return true;
}

bool parse_todo(Ctx& c, const std::string& text, std::vector<Item>& out, std::string& error, bool after_done) {
  out.clear();
  bool fixup_okay = after_done;
  size_t pos = 0;
  int lineno = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos + 1);
    pos = nl == std::string::npos ? text.size() : nl + 1;
    ++lineno;
    Item it;
    std::string why;
    bool ok = parse_line(c, line, it, why);
    if (ok && (it.op == Op::Fixup || it.op == Op::Squash) && !fixup_okay) {
      why = std::string("cannot '") + op_name(it.op) + "' without a previous commit";
      ok = false;
    }
    if (!ok) {
      error = why.empty() ? std::string() : "error: " + why + "\n";
      error += "error: invalid line " + std::to_string(lineno) + ": " + trim_right(line) + "\n";
      return false;
    }
    if (it.op != Op::Comment) {
      if (it.op != Op::Noop && it.op != Op::Drop) fixup_okay = true;
    }
    out.push_back(it);
  }
  return true;
}

std::string format_item(Ctx& c, const Item& it, bool shorten, bool letters) {
  if (it.op == Op::Comment) return it.text + "\n";
  std::string s;
  if (letters && op_letter(it.op)) s += op_letter(it.op);
  else s += op_name(it.op);
  auto oid = [&](const std::string& o) { return shorten ? c.abbrev(o) : o; };
  switch (it.op) {
    case Op::Noop:
    case Op::Break: break;
    case Op::Exec:
    case Op::Label:
    case Op::Reset:
    case Op::UpdateRef: s += " " + it.arg; break;
    case Op::Merge:
      if (it.flag) s += std::string(" -") + it.flag + " " + oid(it.oid);
      s += " " + it.arg;
      break;
    default:
      if (it.flag) s += std::string(" -") + it.flag;
      s += " " + oid(it.oid);
      if (!it.arg.empty()) s += " " + it.arg;
  }
  return s + "\n";
}

std::string format_todo(Ctx& c, const std::vector<Item>& items, bool shorten, bool letters, size_t count) {
  std::string s;
  size_t n = 0;
  for (auto& it : items) {
    if (n++ >= count) break;
    s += format_item(c, it, shorten, letters);
  }
  return s;
}

size_t count_commands(const std::vector<Item>& items) {
  size_t n = 0;
  for (auto& it : items) if (it.op != Op::Comment) ++n;
  return n;
}

std::string todo_help(Ctx& c, const std::string& short_range, const std::string& short_onto, size_t count, bool edit_todo) {
  const char cc = comment_char(c);
  std::string body;
  if (!edit_todo) body += "Rebase " + short_range + " onto " + short_onto + " (" + std::to_string(count) + (count == 1 ? " command)" : " commands)") + "\n";
  body +=
      "\nCommands:\n"
      "p, pick <commit> = use commit\n"
      "r, reword <commit> = use commit, but edit the commit message\n"
      "e, edit <commit> = use commit, but stop for amending\n"
      "s, squash <commit> = use commit, but meld into previous commit\n"
      "f, fixup [-C | -c] <commit> = like \"squash\" but keep only the previous\n"
      "                   commit's log message, unless -C is used, in which case\n"
      "                   keep only this commit's message; -c is same as -C but\n"
      "                   opens the editor\n"
      "x, exec <command> = run command (the rest of the line) using shell\n"
      "b, break = stop here (continue rebase later with 'git rebase --continue')\n"
      "d, drop <commit> = remove commit\n"
      "l, label <label> = label current HEAD with a name\n"
      "t, reset <label> = reset HEAD to a label\n"
      "m, merge [-C <commit> | -c <commit>] <label> [# <oneline>]\n"
      "        create a merge commit using the original merge commit's\n"
      "        message (or the oneline, if no original merge commit was\n"
      "        specified); use -c <commit> to reword the commit message\n"
      "u, update-ref <ref> = track a placeholder for the <ref> to be updated\n"
      "                      to this position in the new commits. The <ref> is\n"
      "                      updated at the end of the rebase\n"
      "\nThese lines can be re-ordered; they are executed from top to bottom.\n";
  const std::string check = c.config.get("rebase.missingcommitscheck").value_or("ignore");
  if (check == "ignore") body += "\nIf you remove a line here THAT COMMIT WILL BE LOST.\n";
  else body += "\nDo not remove any line. Use 'drop' explicitly to remove a commit.\n";
  if (edit_todo)
    body += "\nYou are editing the todo file of an ongoing interactive rebase.\nTo continue rebase after editing, run:\n    git rebase --continue\n\n";
  else
    body += "\nHowever, if you remove everything, the rebase will be aborted.\n\n";
  std::string out, line;
  std::istringstream ss(body);
  while (std::getline(ss, line)) out += line.empty() ? std::string(1, cc) + "\n" : std::string(1, cc) + " " + line + "\n";
  return out;
}

// ---------------------------------------------------------------------------
// the list a rebase starts from
// ---------------------------------------------------------------------------
namespace {

// Two commits make "the same patch" when their changes against their parents read alike, whitespace aside.
std::string patch_id(Ctx& c, const GitCommit& cm) {
  std::string from = cm.parents.empty() ? "" : read_commit(c, cm.parents[0]).tree;
  DiffOptions o;
  o.format = DiffOptions::Patch;
  o.renames = false;
  o.full_index = true;
  auto pairs = diff_trees(c, from, cm.tree, Pathspec{}, o);
  std::ostringstream ss;
  print_diff(c, pairs, o, ss);
  std::string key, line;
  std::istringstream in(ss.str());
  while (std::getline(in, line)) {
    if (line.rfind("index ", 0) == 0 || line.rfind("@@", 0) == 0) continue;
    key += whitespace_free(line) + "\n";
  }
  return hash_blob(key);
}

// The commits of tip..hidden oldest first in git's --topo-order --reverse: a commit follows all its children's
// lines of history, and the walk finishes a branch before it goes back to the one it left.
std::vector<GitCommit> range_commits(Ctx& c, const std::string& tip, const std::string& hidden, bool merges) {
  WalkOptions w;
  w.tips = {tip};
  if (!hidden.empty()) w.hide = {hidden};
  std::vector<GitCommit> all = walk(c, w);
  std::map<std::string, const GitCommit*> by_oid;
  std::map<std::string, int> indegree;
  for (auto& cm : all) {
    by_oid[cm.oid] = &cm;
    indegree[cm.oid] = 1;
  }
  for (auto& cm : all) for (auto& po : cm.parents) if (indegree.count(po)) ++indegree[po];
  std::vector<std::string> stack;
  for (auto& cm : all) if (indegree[cm.oid] == 1) stack.insert(stack.begin(), cm.oid);
  std::vector<GitCommit> newest_first;
  while (!stack.empty()) {
    std::string oid = stack.back();
    stack.pop_back();
    const GitCommit* cm = by_oid[oid];
    for (auto& po : cm->parents) {
      auto it = indegree.find(po);
      if (it == indegree.end() || it->second == 0) continue;
      if (--it->second == 1) stack.push_back(po);
    }
    indegree[oid] = 0;
    if (merges || cm->parents.size() < 2) newest_first.push_back(*cm);
  }
  std::reverse(newest_first.begin(), newest_first.end());
  return newest_first;
}

}  // namespace

// The names `label` lines give: a label is made of letters, digits and dashes, and is not used twice (case aside).
struct LabelState {
  std::map<std::string, std::string> commit2label;
  std::set<std::string> taken;   // lower case

  const std::string& label_oid(Ctx& c, const std::string& oid, const std::string& label_in, bool has_label) {
    auto it = commit2label.find(oid);
    if (it != commit2label.end()) return it->second;
    std::string label;
    auto lower = [](std::string v) { for (auto& ch : v) ch = (char)std::tolower((unsigned char)ch); return v; };
    if (!has_label) {
      label = c.abbrev(oid);
      if (taken.count(lower(label))) {
        label = oid;
        for (size_t i = c.abbrev(oid).size() + 1; i < oid.size(); ++i) {
          if (!taken.count(lower(oid.substr(0, i)))) { label = oid.substr(0, i); break; }
        }
      }
    } else {
      for (unsigned char ch : label_in) {
        if ((ch & 0x80) || std::isalnum(ch)) label += (char)ch;
        else if (!label.empty() && label.back() != '-') label += '-';
      }
      if (label.empty()) label = "rev-" + c.abbrev(oid);
      auto full_hex = [](const std::string& v) {
        return v.size() == 40 && std::all_of(v.begin(), v.end(), [](char ch) { return std::isxdigit((unsigned char)ch); });
      };
      if (full_hex(label) || label == "#" || taken.count(lower(label))) {
        const std::string stem = label;
        for (int i = 2;; ++i) {
          label = stem + "-" + std::to_string(i);
          if (!taken.count(lower(label))) break;
        }
      }
    }
    taken.insert(lower(label));
    return commit2label[oid] = label;
  }
};

// `--rebase-merges`: label, reset and merge lines that rebuild the shape of the history.
std::vector<Item> make_script_with_merges(Ctx& c, const std::vector<GitCommit>& commits, const std::set<std::string>& same,
                                          const std::string& base_oid, const ScriptOptions& o) {
  const char cc = comment_char(c);
  LabelState state;
  std::set<std::string> interesting, child_seen, shown;
  std::map<std::string, Item> commit2todo;
  std::vector<std::string> tips;
  if (!base_oid.empty()) {
    state.commit2label[base_oid] = "onto";
    state.taken.insert("onto");
  }
  bool skipped = false;
  // first phase: a line for each commit, and a label for the tip of every merged branch
  for (auto& cm : commits) {
    interesting.insert(cm.oid);
    const bool empty = is_original_empty(c, cm);
    if (!empty && same.count(cm.oid)) {
      c.err << "warning: skipped previously applied commit " << c.abbrev(cm.oid) << "\n";
      skipped = true;
      continue;
    }
    if (empty && !o.keep_empty) continue;
    const std::string oneline = todo_subject(c, cm);
    Item it;
    if (cm.parents.size() < 2) {
      it.op = Op::Pick;
      it.oid = cm.oid;
      it.arg = oneline + (empty ? std::string(" ") + cc + " empty" : std::string());
      commit2todo[cm.oid] = it;
      continue;
    }
    std::string label;
    size_t q1 = oneline.rfind("Merge ", 0) == 0 ? oneline.find('\'', 6) : std::string::npos;
    size_t q2 = q1 == std::string::npos ? q1 : oneline.find('\'', q1 + 1);
    size_t from = oneline.rfind("Merge pull request ", 0) == 0 ? oneline.find(" from ", 19) : std::string::npos;
    if (q2 != std::string::npos) label = oneline.substr(q1 + 1, q2 - q1 - 1);
    else if (from != std::string::npos) label = oneline.substr(from + 6);
    else label = oneline;
    it.op = Op::Merge;
    it.flag = 'C';
    it.oid = cm.oid;
    std::string heads;
    for (size_t i = 1; i < cm.parents.size(); ++i) {
      const std::string& po = cm.parents[i];
      if (!heads.empty()) heads += " ";
      if (!interesting.count(po)) {
        heads += state.label_oid(c, po, "", false);
        continue;
      }
      tips.push_back(po);
      heads += state.label_oid(c, po, label, true);
    }
    it.arg = heads + " " + cc + " " + oneline;
    commit2todo[cm.oid] = it;
  }
  if (skipped && c.config.get_bool("advice.skippedcherrypicks", true))
    c.err << "hint: use --reapply-cherry-picks to include skipped commits\n"
             "hint: Disable this message with \"git config advice.skippedCherryPicks false\"\n";
  // second phase: label the branch points, and the tip of the branch being rebased
  for (size_t i = 0; i < commits.size(); ++i) {
    for (auto& po : commits[i].parents) {
      if (!interesting.count(po)) continue;
      if (!child_seen.insert(po).second) state.label_oid(c, po, "branch-point", true);
    }
    if (i + 1 == commits.size()) tips.push_back(commits[i].oid);
  }
  // third phase: walk back from each tip, so a branch is listed together
  std::vector<Item> out;
  auto line = [&](Op op, const std::string& arg) {
    Item it;
    it.op = op;
    it.arg = arg;
    out.push_back(it);
  };
  auto blank = [&] { out.push_back(Item{Op::Comment, "", "", 0, ""}); };
  auto text = [&](const std::string& t) {
    Item it;
    it.op = Op::Comment;
    it.text = t;
    out.push_back(it);
  };
  line(Op::Label, "onto");
  std::map<std::string, const GitCommit*> by_oid;
  for (auto& cm : commits) by_oid[cm.oid] = &cm;
  for (auto& tip : tips) {
    if (shown.count(tip)) continue;
    auto lab = state.commit2label.find(tip);
    if (lab != state.commit2label.end()) {
      blank();
      text(std::string(1, cc) + " Branch " + lab->second);
    } else {
      blank();
    }
    std::vector<std::string> list;
    std::string cur = tip;
    bool root = false;
    while (interesting.count(cur) && !shown.count(cur)) {
      list.insert(list.begin(), cur);
      const GitCommit* cm = by_oid.count(cur) ? by_oid[cur] : nullptr;
      if (!cm || cm->parents.empty()) {
        root = true;
        break;
      }
      cur = cm->parents[0];
    }
    if (root) {
      line(Op::Reset, o.rebase_cousins || o.root_with_onto ? "[new root]" : "onto");
    } else {
      std::string to;
      auto e = state.commit2label.find(cur);
      if (e != state.commit2label.end()) to = e->second;
      else if (!o.rebase_cousins) to = state.label_oid(c, cur, "", false);
      if (to.empty() || to == "onto") line(Op::Reset, "onto");
      else line(Op::Reset, to + " " + cc + " " + todo_subject(c, read_commit(c, cur)));
    }
    for (auto& oid : list) {
      auto t = commit2todo.find(oid);
      if (t != commit2todo.end()) out.push_back(t->second);
      auto l = state.commit2label.find(oid);
      if (l != state.commit2label.end()) line(Op::Label, l->second);
      shown.insert(oid);
    }
  }
  return out;
}

std::vector<Item> make_script(Ctx& c, const std::string& left_of, const std::string& head, const ScriptOptions& o) {
  std::vector<Item> items;
  std::vector<GitCommit> picks;
  picks = range_commits(c, head, left_of, o.rebase_merges);
  // commits whose change is already upstream are left out (unless asked to replay them)
  std::set<std::string> same;
  if (!o.reapply_cherry_picks && !left_of.empty() && !picks.empty()) {
    std::map<std::string, std::vector<std::string>> mine;
    for (auto& cm : picks) {
      if (cm.parents.size() > 1 || is_original_empty(c, cm)) continue;
      mine[patch_id(c, cm)].push_back(cm.oid);
    }
    if (!mine.empty()) {
      WalkOptions w;
      w.tips = {left_of};
      w.hide = {head};
      w.max_parents = 1;
      size_t matched = 0;
      for (auto& cm : walk(c, w)) {
        auto it = mine.find(patch_id(c, cm));
        if (it == mine.end() || it->second.empty()) continue;
        for (auto& id : it->second) same.insert(id);
        matched += it->second.size();
        it->second.clear();
        bool all = true;
        for (auto& [k, v] : mine) if (!v.empty()) all = false;
        if (all) break;
      }
      (void)matched;
    }
  }
  if (o.rebase_merges) {
    // the merge base is what `onto` names in the labels
    std::string base_oid;
    if (!left_of.empty()) {
      auto bases = merge_bases(c, left_of, head);
      if (!bases.empty()) base_oid = bases[0];
    }
    return make_script_with_merges(c, picks, same, base_oid, o);
  }
  bool skipped = false;
  for (auto& cm : picks) {
    const bool empty = is_original_empty(c, cm);
    if (!empty && same.count(cm.oid)) {
      c.err << "warning: skipped previously applied commit " << c.abbrev(cm.oid) << "\n";
      skipped = true;
      continue;
    }
    if (empty && !o.keep_empty) {
      Item it;
      it.op = Op::Comment;
      it.text = std::string(1, comment_char(c)) + " " + (o.letters ? "p" : "pick") + " " + cm.oid + " " + todo_subject(c, cm) + " " + comment_char(c) + " empty";
      items.push_back(it);
      continue;
    }
    Item it;
    it.op = Op::Pick;
    it.oid = cm.oid;
    it.arg = todo_subject(c, cm);
    if (empty) it.arg += std::string(" ") + comment_char(c) + " empty";
    items.push_back(it);
  }
  if (skipped && c.config.get_bool("advice.skippedcherrypicks", true))
    c.err << "hint: use --reapply-cherry-picks to include skipped commits\n"
             "hint: Disable this message with \"git config advice.skippedCherryPicks false\"\n";
  return items;
}

void rearrange_squash(Ctx& c, std::vector<Item>& items) {
  const size_t n = items.size();
  std::vector<std::string> subjects(n);
  std::map<std::string, size_t> by_subject, by_commit;
  std::vector<long> next(n, -1), tail(n, -1);
  bool rearranged = false;
  auto fixupish = [](const std::string& s, size_t& skip) {
    for (const char* p : {"fixup! ", "amend! ", "squash! "}) {
      size_t len = std::string(p).size();
      if (s.compare(0, len, p) == 0) { skip = len; return true; }
    }
    return false;
  };
  for (size_t i = 0; i < n; ++i) if (items[i].commit_op()) by_commit[items[i].oid] = i;
  for (size_t i = 0; i < n; ++i) {
    Item& it = items[i];
    if (!it.commit_op()) continue;
    std::string subject = subject_of(read_commit(c, it.oid).message);
    subjects[i] = subject;
    size_t skip = 0;
    long i2 = -2;
    if (fixupish(subject, skip)) {
      std::string p = subject.substr(skip);
      for (;;) {
        size_t sp = 0;
        while (sp < p.size() && std::isspace((unsigned char)p[sp])) ++sp;
        p = p.substr(sp);
        size_t s2 = 0;
        if (!fixupish(p, s2)) break;
        p = p.substr(s2);
      }
      auto bs = by_subject.find(p);
      if (bs != by_subject.end()) i2 = (long)bs->second;
      else if (p.find(' ') == std::string::npos) {
        auto r = resolve(c, p + "^{commit}");
        auto bc = r ? by_commit.find(*r) : by_commit.end();
        if (bc != by_commit.end()) i2 = (long)bc->second;
      }
      if (i2 == -2) {
        long k;
        for (k = 0; k < (long)i; ++k) if (!subjects[(size_t)k].empty() && subjects[(size_t)k].compare(0, p.size(), p) == 0) break;
        i2 = k == (long)i ? -1 : k;
      }
      if (i2 >= 0) {
        rearranged = true;
        if (subject.rfind("fixup!", 0) == 0) it.op = Op::Fixup;
        else if (subject.rfind("amend!", 0) == 0) { it.op = Op::Fixup; it.flag = 'C'; }
        else it.op = Op::Squash;
        if (tail[(size_t)i2] < 0) { next[i] = next[(size_t)i2]; next[(size_t)i2] = (long)i; }
        else { next[i] = next[(size_t)tail[(size_t)i2]]; next[(size_t)tail[(size_t)i2]] = (long)i; }
        tail[(size_t)i2] = (long)i;
      } else if (!by_subject.count(subject)) {
        by_subject[subject] = i;
      }
    } else if (!by_subject.count(subject)) {
      by_subject[subject] = i;
    }
  }
  if (!rearranged) return;
  std::vector<Item> out;
  std::vector<bool> placed(n, false), chained(n, false);
  for (size_t j = 0; j < n; ++j) if (next[j] >= 0) chained[(size_t)next[j]] = true;
  for (size_t i = 0; i < n; ++i) {
    if (placed[i] || chained[i]) continue;
    for (long cur = (long)i; cur >= 0; cur = next[(size_t)cur]) {
      if (placed[(size_t)cur]) break;
      placed[(size_t)cur] = true;
      out.push_back(items[(size_t)cur]);
    }
  }
  for (size_t i = 0; i < n; ++i) if (!placed[i]) out.push_back(items[i]);
  items = out;
}

void add_update_ref_commands(Ctx& c, std::vector<Item>& items, const std::string& skip_ref) {
  // branches (not the one being rebased, nor one another work tree has) that point at a commit being replayed
  std::set<std::string> busy;
  if (!skip_ref.empty()) busy.insert(skip_ref);
  const std::string wt_dir = c.repo->common_dir() + "/worktrees";
  if (auto names = c.host().list_dir(wt_dir); names.ok()) {
    for (auto& n : *names) {
      auto h = c.host().read_file(wt_dir + "/" + n + "/HEAD");
      if (!h.ok()) continue;
      std::string t((const char*)h->data(), h->size());
      if (t.rfind("ref: ", 0) == 0) busy.insert(t.substr(5, t.find('\n') == std::string::npos ? std::string::npos : t.find('\n') - 5));
    }
  }
  std::map<std::string, std::vector<std::string>> at;
  for (auto& [name, oid] : all_refs(c)) if (name.rfind("refs/heads/", 0) == 0 && !busy.count(name)) at[oid].push_back(name);
  if (at.empty()) return;
  std::vector<Item> out;
  std::vector<std::string> pending;
  for (size_t i = 0; i < items.size(); ++i) {
    out.push_back(items[i]);
    if (!items[i].commit_op()) continue;
    auto it = at.find(items[i].oid);
    if (it != at.end()) for (auto& r : it->second) pending.push_back(r);
    const bool chain_goes_on = i + 1 < items.size() && (items[i + 1].op == Op::Fixup || items[i + 1].op == Op::Squash);
    if (chain_goes_on) continue;
    for (auto& r : pending) {
      Item u;
      u.op = Op::UpdateRef;
      u.arg = r;
      out.push_back(u);
    }
    if (!pending.empty()) {
      Item gap;
      gap.op = Op::Comment;
      out.push_back(gap);
    }
    pending.clear();
  }
  items = out;
}

void add_exec_commands(std::vector<Item>& items, const std::vector<std::string>& cmds) {
  if (cmds.empty()) return;
  std::vector<Item> out;
  auto put = [&] {
    for (auto& cmd : cmds) {
      Item e;
      e.op = Op::Exec;
      e.arg = cmd;
      out.push_back(e);
    }
  };
  bool insert = false;
  for (auto& it : items) {
    if (insert && it.op != Op::Fixup && it.op != Op::Squash) { put(); insert = false; }
    out.push_back(it);
    if (it.op == Op::Pick || it.op == Op::Merge) insert = true;
  }
  if (insert) put();
  items = out;
}

}  // namespace aios_git::rebase
