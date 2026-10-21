// Programs git starts: the shell wrapper that gives them standard input and captured output, the editor, and hooks.
#include <cstdlib>
#include <functional>

#include "aios/hal/host.hpp"
#include "aios/hal/process.hpp"
#include "cli_rewrite.hpp"

namespace aios_git {

using namespace aios;

namespace {

std::string slurp(Ctx& c, const std::string& path) {
  auto f = c.host().read_file(path);
  if (!f.ok()) return {};
  return std::string((const char*)f->data(), f->size());
}

}  // namespace

std::vector<std::pair<std::string, std::string>> child_env(Ctx& c) {
  std::vector<std::pair<std::string, std::string>> out;
  for (auto& [k, v] : c.env) out.push_back({k, v});
  return out;
}

ProcResult run_proc(Ctx& c, const ProcSpec& spec) {
  ProcResult r;
  const std::string sh = hal::find_program({"sh", "aios-sh"});
  if (sh.empty()) return r;
  auto scratch = hal::scratch_path(".proc");
  if (!scratch.ok()) return r;
  const std::string base = *scratch, in = base + ".in", out = base + ".out", err = base + ".err";
  auto& h = c.host();
  auto cleanup = [&] { for (const auto& f : {base, in, out, err}) (void)h.remove(f); };
  const std::string& data = spec.input.value_or(std::string());
  if (spec.input) {
    if (!h.write_file(in, ByteSpan((const uint8_t*)data.data(), data.size())).ok()) { cleanup(); return r; }
  }
  hal::ProcessSpawn ps;
  // the wrapper takes standard input from a file (the process layer has none), standard output and error to files
  const std::string redirect = std::string("exec <") + (spec.input ? "\"$AIOS_GIT_IN\"" : "/dev/null") + " >\"$AIOS_GIT_OUT\" 2>" +
                               (spec.merge_output ? "&1" : "\"$AIOS_GIT_ERR\"") + "; unset AIOS_GIT_IN AIOS_GIT_OUT AIOS_GIT_ERR; ";
  if (!spec.script.empty()) {
    ps.argv = {sh, "-c", redirect + spec.script + " \"$@\"", spec.script};
    for (auto& a : spec.argv) ps.argv.push_back(a);
  } else {
    if (spec.argv.empty()) { cleanup(); return r; }
    ps.argv = {sh, "-c", redirect + "exec \"$@\"", "sh"};
    for (auto& a : spec.argv) ps.argv.push_back(a);
  }
  ps.working_dir = spec.dir;
  ps.env = child_env(c);
  for (auto& e : spec.env) ps.env.push_back(e);
  ps.env.push_back({"AIOS_GIT_IN", in});
  ps.env.push_back({"AIOS_GIT_OUT", spec.merge_output ? err : out});
  ps.env.push_back({"AIOS_GIT_ERR", err});
  ps.unset_env = spec.unset_env;
  auto p = hal::spawn_process(ps);
  if (!p.ok()) { cleanup(); return r; }
  r.started = true;
  auto st = hal::wait_process(*p, 0xffffffffu);
  hal::release_process(*p);
  r.code = st.ok() && st->exited ? (st->terminating_signal ? 128 + st->terminating_signal : st->exit_code) : 1;
  r.out = spec.merge_output ? std::string() : slurp(c, out);
  r.err = slurp(c, err);
  cleanup();
  return r;
}

std::string editor_command(Ctx& c, bool sequence) {
  std::string e;
  if (sequence) {
    e = c.getenv("GIT_SEQUENCE_EDITOR");
    if (e.empty()) e = c.config.get("sequence.editor").value_or("");
  }
  if (e.empty()) e = c.getenv("GIT_EDITOR");
  if (e.empty()) e = c.config.get("core.editor").value_or("");
  if (e.empty()) e = c.getenv("VISUAL");
  if (e.empty()) e = c.getenv("EDITOR");
  return e;
}

void edit_file(Ctx& c, const std::string& path, bool sequence, const std::string& what) {
  std::string e = editor_command(c, sequence);
  if (e.empty()) {
    const std::string term = c.getenv("TERM");
    if (term.empty() || term == "dumb")
      die(c, "Terminal is dumb, but EDITOR unset");
    e = "vi";
  }
  if (e == ":") return;
  ProcSpec s;
  s.script = e;
  s.argv = {path};
  s.dir = c.top.empty() ? "" : c.top;
  ProcResult r = run_proc(c, s);
  c.out << r.out;
  c.err << r.err;
  if (!r.started) die(c, "could not launch " + what + " '" + e + "'");
  if (r.code != 0) {
    c.err << "error: There was a problem with " << what << " '" << e << "'.\n";
    throw Exit{1};
  }
}

std::string find_hook(Ctx& c, const std::string& name, bool warn) {
  std::string dir;
  if (auto hp = c.config.get("core.hookspath")) {
    dir = *hp;
    if (dir.rfind("~/", 0) == 0) dir = slashes(c.getenv("HOME")) + dir.substr(1);
    dir = is_absolute(dir) ? dir : join(c.top.empty() ? c.repo->git_dir() : c.top, dir);
  } else {
    dir = c.repo->common_dir() + "/hooks";
  }
  const std::string path = normalize_path(dir + "/" + name);
  if (!c.host().exists(path)) return {};
  auto st = c.host().stat(path);
  if (!st.ok() || st->is_dir) return {};
  if (hal::find_program({path}).empty()) {
    if (warn && c.config.get_bool("advice.ignoredhook", true)) {
      const std::string shown = !c.top.empty() && path.rfind(c.top + "/", 0) == 0 ? path.substr(c.top.size() + 1) : path;
      c.err << "hint: The '" << shown << "' hook was ignored because it's not set as executable.\n"
            << "hint: You can disable this warning with `git config advice.ignoredHook false`.\n";
    }
    return {};
  }
  return path;
}

HookResult run_hook(Ctx& c, const std::string& name, const std::vector<std::string>& args, const HookOptions& o) {
  HookResult res;
  if (!c.repo) return res;
  const std::string path = find_hook(c, name);
  if (path.empty()) return res;
  ProcSpec s;
  s.argv = {path};
  for (auto& a : args) s.argv.push_back(a);
  s.dir = c.top.empty() ? c.repo->git_dir() : c.top;
  s.env = o.env;
  if (c.top.empty() && !c.env.count("GIT_DIR")) s.env.push_back({"GIT_DIR", "."});
  if (!c.env.count("GIT_PREFIX")) s.env.push_back({"GIT_PREFIX", c.prefix});
  s.input = o.input;
  s.merge_output = !o.capture_stdout;
  ProcResult r = run_proc(c, s);
  c.err.flush();
  c.err << r.err;
  res.ran = true;
  res.code = r.started ? r.code : 1;
  res.out = r.out;
  return res;
}

void run_hook_or_die(Ctx& c, const std::string& name, const std::vector<std::string>& args, const HookOptions& o) {
  HookResult r = run_hook(c, name, args, o);
  if (r.ran && r.code != 0) throw Exit{r.code};
}

std::string git_dir_path_for_hook(Ctx& c, const std::string& name) {
  const std::string dir = c.repo->git_dir();
  return (!c.top.empty() && dir == c.top + "/.git" ? std::string(".git") : dir) + "/" + name;
}

namespace {
// `temp_index`: a commit of all changes or of paths works on a copy of the index, which git names by its lock file.
std::vector<std::pair<std::string, std::string>> commit_hook_env(Ctx& c, bool temp_index = false) {
  return {{"GIT_INDEX_FILE", temp_index ? c.repo->git_dir() + "/index.lock" : git_dir_path_for_hook(c, "index")}, {"GIT_EDITOR", ":"}};
}
}  // namespace

int run_pre_commit_hook(Ctx& c, const std::string& name, bool skip, bool temp_index) {
  if (skip || !c.repo) return 0;
  HookOptions o;
  o.env = commit_hook_env(c, temp_index);
  HookResult r = run_hook(c, name, {}, o);
  return r.ran ? r.code : 0;
}

MessageHooks run_message_hooks(Ctx& c, const std::string& file, const std::string& message, const std::string& source,
                               const std::string& sha, bool skip_commit_msg, bool temp_index) {
  MessageHooks out;
  out.message = message;
  if (!c.repo) return out;
  const bool has_prepare = !find_hook(c, "prepare-commit-msg", false).empty();
  const bool has_commit_msg = !skip_commit_msg;
  if (!has_prepare && (!has_commit_msg || find_hook(c, "commit-msg", false).empty())) return out;
  c.write_git_file(file, message);
  const std::string rel = git_dir_path_for_hook(c, file);
  HookOptions o;
  o.env = commit_hook_env(c, temp_index);
  if (has_prepare) {
    std::vector<std::string> args{rel};
    if (!source.empty()) args.push_back(source);
    if (!sha.empty()) args.push_back(sha);
    HookResult r = run_hook(c, "prepare-commit-msg", args, o);
    if (r.ran && r.code != 0) { out.code = r.code; return out; }
  }
  if (has_commit_msg) {
    HookResult r = run_hook(c, "commit-msg", {rel}, o);
    if (r.ran && r.code != 0) { out.code = r.code; return out; }
  }
  out.message = c.read_git_file(file).value_or(message);
  return out;
}

void run_post_commit_hooks(Ctx& c, bool amend, const std::string& old_oid, const std::string& new_oid) {
  if (!c.repo) return;
  HookOptions o;
  o.env = commit_hook_env(c);
  run_hook(c, "post-commit", {}, o);
  if (amend) {
    HookOptions w;
    w.input = old_oid + " " + new_oid + "\n";
    run_hook(c, "post-rewrite", {"amend"}, w);
  }
}

void run_post_merge_hook(Ctx& c, bool squash) {
  if (c.repo) run_hook(c, "post-merge", {squash ? "1" : "0"});
}

int run_post_checkout_hook(Ctx& c, const std::string& old_oid, const std::string& new_oid, bool branch) {
  if (!c.repo) return 0;
  HookResult r = run_hook(c, "post-checkout", {old_oid.empty() ? std::string(40, '0') : old_oid, new_oid, branch ? "1" : "0"});
  return r.ran ? r.code : 0;
}

void with_ref_transaction(Ctx& c, const std::vector<RefUpdate>& updates, const std::function<void()>& apply) {
  if (!c.repo || updates.empty() || find_hook(c, "reference-transaction").empty()) {
    apply();
    return;
  }
  const std::string zero(40, '0');
  std::string lines;
  for (auto& u : updates) lines += (u.old_oid.empty() ? zero : u.old_oid) + " " + (u.new_oid.empty() ? zero : u.new_oid) + " " + u.name + "\n";
  HookOptions o;
  o.input = lines;
  HookResult prepared = run_hook(c, "reference-transaction", {"prepared"}, o);
  if (prepared.ran && prepared.code != 0) {
    run_hook(c, "reference-transaction", {"aborted"}, o);
    c.err << "fatal: ref updates aborted by hook\n";
    throw Exit{128};
  }
  try {
    apply();
  } catch (...) {
    run_hook(c, "reference-transaction", {"aborted"}, o);
    throw;
  }
  run_hook(c, "reference-transaction", {"committed"}, o);
}

}  // namespace aios_git
