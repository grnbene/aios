// aios-git: the entry point. Global options, the command table, aliases.
#include "git_cli.hpp"

#include <cstdlib>
#include <exception>

#include "aios/hal/host.hpp"
#include "change.hpp"
#include "cli.hpp"
#include "hist_util.hpp"
#include "cli_rewrite.hpp"
#include "oplog.hpp"
#include "pager.hpp"

#ifdef _WIN32
#include <windows.h>
#else
extern char** environ;
#endif

namespace aios_git {

const std::map<std::string, Command>& commands() {
  static const std::map<std::string, Command> m = {
      {"init", cmd_init}, {"status", cmd_status}, {"add", cmd_add}, {"rm", cmd_rm}, {"mv", cmd_mv}, {"commit", cmd_commit},
      {"reset", cmd_reset}, {"restore", cmd_restore}, {"clean", cmd_clean}, {"log", cmd_log}, {"whatchanged", cmd_whatchanged}, {"show", cmd_show},
      {"diff", cmd_diff}, {"rev-list", cmd_rev_list}, {"rev-parse", cmd_rev_parse}, {"branch", cmd_branch}, {"tag", cmd_tag},
      {"checkout", cmd_checkout}, {"switch", cmd_switch}, {"merge", cmd_merge}, {"merge-base", cmd_merge_base},
      {"cherry-pick", cmd_cherry_pick}, {"revert", cmd_revert}, {"stash", cmd_stash}, {"config", cmd_config},
      {"cat-file", cmd_cat_file}, {"hash-object", cmd_hash_object}, {"mktree", cmd_mktree}, {"write-tree", cmd_write_tree},
      {"read-tree", cmd_read_tree}, {"checkout-index", cmd_checkout_index}, {"update-index", cmd_update_index}, {"commit-tree", cmd_commit_tree},
      {"update-ref", cmd_update_ref}, {"symbolic-ref", cmd_symbolic_ref}, {"ls-files", cmd_ls_files}, {"ls-tree", cmd_ls_tree},
      {"notes", cmd_notes}, {"apply", cmd_apply}, {"archive", cmd_archive}, {"worktree", cmd_worktree}, {"show-ref", cmd_show_ref},
      {"for-each-ref", cmd_for_each_ref}, {"clone", cmd_clone}, {"fetch", cmd_fetch}, {"pull", cmd_pull}, {"push", cmd_push},
      {"ls-remote", cmd_ls_remote}, {"remote", cmd_remote}, {"bundle", cmd_bundle}, {"http-backend", cmd_http_backend},
      {"upload-pack", cmd_upload_pack}, {"receive-pack", cmd_receive_pack}, {"git-upload-pack", cmd_upload_pack},
      {"git-receive-pack", cmd_receive_pack}, {"fsck", cmd_fsck}, {"gc", cmd_gc}, {"reflog", cmd_reflog}, {"repack", cmd_repack}, {"check-ignore", cmd_check_ignore},
      {"var", cmd_var}, {"grep", cmd_grep}, {"sparse-checkout", cmd_sparse_checkout}, {"blocks", cmd_blocks}, {"fsmonitor--daemon", cmd_fsmonitor_daemon},
      {"outline", cmd_outline}, {"journal", cmd_journal},
      {"verify-commit", cmd_verify_commit}, {"rebase", cmd_rebase},
      AIOS_GIT_HISTORY_COMMANDS
      {"shortlog", cmd_shortlog}, {"count-objects", cmd_count_objects}, {"format-patch", cmd_format_patch},
      {"submodule", cmd_submodule},
      // the change layer and the operation log (not git commands: aios-git's own)
      {"change", cmd_change}, {"new", cmd_new}, {"op", oplog::cmd_op}, {"undo", oplog::cmd_undo}, {"redo", oplog::cmd_redo}, {"stack", cmd_stack}};
  return m;
}

namespace {

const char* kVersion = "git version 2.45.0 (aios)";

void help(std::ostream& out) {
  out << "usage: git [-v | --version] [-h | --help] [-C <path>] [-c <name>=<value>]\n"
         "           [--git-dir=<path>] [--work-tree=<path>] <command> [<args>]\n\n"
         "aios-git: git on aios' own git library. Commands:\n";
  int col = 0;
  for (auto& [name, fn] : commands()) {
    (void)fn;
    if (name.rfind("git-", 0) == 0) continue;
    out << (col == 0 ? "   " : "") << name << std::string(name.size() < 16 ? 16 - name.size() : 1, ' ');
    if (++col == 5) { out << "\n"; col = 0; }
  }
  if (col) out << "\n";
}

std::map<std::string, std::string> process_env() {
  std::map<std::string, std::string> env;
#ifdef _WIN32
  wchar_t* block = ::GetEnvironmentStringsW();
  if (!block) return env;
  for (wchar_t* p = block; *p; p += wcslen(p) + 1) {
    int n = ::WideCharToMultiByte(CP_UTF8, 0, p, -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) ::WideCharToMultiByte(CP_UTF8, 0, p, -1, s.data(), n, nullptr, nullptr);
    size_t eq = s.find('=', 1);   // "=C:=C:\x" entries start with '='
    if (eq == std::string::npos || s[0] == '=') continue;
    env[s.substr(0, eq)] = s.substr(eq + 1);
  }
  ::FreeEnvironmentStringsW(block);
#else
  for (char** e = environ; e && *e; ++e) {
    std::string s = *e;
    size_t eq = s.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    env[s.substr(0, eq)] = s.substr(eq + 1);
  }
#endif
  return env;
}

std::string kernel_root(Ctx& c) {
  // a scratch root for the in-process kernel: nothing of the repository lives there
  std::string t = c.getenv("TMPDIR");
  if (t.empty()) t = c.getenv("TEMP");
  if (t.empty()) t = c.getenv("TMP");
#ifdef _WIN32
  if (t.empty()) t = c.getenv("LOCALAPPDATA").empty() ? "C:/Windows/Temp" : c.getenv("LOCALAPPDATA") + "/Temp";
#else
  if (t.empty()) t = "/tmp";
#endif
  return slashes(t) + "/aios-git";
}

}  // namespace

int run(const std::vector<std::string>& raw_args, const std::string& start_cwd, std::ostream& out, std::ostream& err,
        std::istream& in, const std::map<std::string, std::string>* env) {
  // what commands write: `out` itself, or out through the colors and the pager (Terminal)
  std::ostream tout(out.rdbuf());
  Ctx c(tout, err, in);
  PagerOptions pager_opt;
  c.env = env ? *env : process_env();
  c.cwd = normalize_path(slashes(start_cwd));
  try {
    size_t i = 1;
    for (; i < raw_args.size(); ++i) {
      const std::string& a = raw_args[i];
      if (a == "-C") {
        if (i + 1 >= raw_args.size()) usage(c, "no directory given for -C");
        std::string d = raw_args[++i];
        if (!d.empty()) {
          std::string to = c.abs_path(d);
          auto st = aios::hal::process_host().stat(to);
          if (!st.ok() || !st->is_dir) die(c, "cannot change to '" + d + "': No such file or directory");
          c.cwd = to;
        }
        continue;
      }
      if (a == "-c") {
        if (i + 1 >= raw_args.size()) usage(c, "-c expects a configuration string");
        std::string kv = raw_args[++i];
        size_t eq = kv.find('=');
        if (eq == std::string::npos) c.cli_config.push_back({kv, "\x01"});
        else c.cli_config.push_back({kv.substr(0, eq), kv.substr(eq + 1)});
        continue;
      }
      if (a.rfind("--git-dir=", 0) == 0) { c.opt_git_dir = slashes(a.substr(10)); continue; }
      if (a == "--git-dir") { if (i + 1 >= raw_args.size()) usage(c, "no directory given for --git-dir"); c.opt_git_dir = slashes(raw_args[++i]); continue; }
      if (a.rfind("--work-tree=", 0) == 0) { c.opt_work_tree = slashes(a.substr(12)); continue; }
      if (a == "--work-tree") { if (i + 1 >= raw_args.size()) usage(c, "no directory given for --work-tree"); c.opt_work_tree = slashes(raw_args[++i]); continue; }
      if (a == "--bare") { c.opt_git_dir = c.cwd; continue; }
      if (a == "--no-pager" || a == "-P") { pager_opt.never = true; continue; }
      if (a == "-p" || a == "--paginate") { pager_opt.force = true; continue; }
      if (a == "--no-replace-objects" || a == "--no-optional-locks" || a == "--no-advice" || a == "--no-lazy-fetch")
        continue;
      if (a == "--version" || a == "-v" || a == "version") {
        out << kVersion << "\n";
        return 0;
      }
      if (a == "--help" || a == "-h" || a == "help") {
        help(out);
        return 0;
      }
      if (a == "--exec-path" || a.rfind("--exec-path=", 0) == 0 || a == "--html-path" || a == "--man-path" || a == "--info-path" ||
          a.rfind("--namespace", 0) == 0 || a == "--literal-pathspecs" || a == "--glob-pathspecs" || a == "--noglob-pathspecs" ||
          a == "--icase-pathspecs" || a.rfind("--config-env", 0) == 0 || a == "--list-cmds" || a.rfind("--list-cmds=", 0) == 0) {
        unsupported(c, "the option " + a);
      }
      if (!a.empty() && a[0] == '-') usage(c, "unknown option: " + a);
      break;
    }
    c.opt_git_dir_given = c.opt_git_dir;
    if (!c.opt_git_dir.empty()) c.opt_git_dir = c.abs_path(c.opt_git_dir);
    if (!c.opt_work_tree.empty()) c.opt_work_tree = c.abs_path(c.opt_work_tree);
    if (i >= raw_args.size()) {
      help(err);
      return 1;
    }
    c.args.assign(raw_args.begin() + (long)i, raw_args.end());
    // the kernel this command runs on: storage through hal::Host, nothing else needed
#ifdef _WIN32
    _putenv_s("AIOS_NO_ACCEL", "1");
#else
    setenv("AIOS_NO_ACCEL", "1", 1);
#endif
    aios::KernelConfig kc;
    kc.root = kernel_root(c);
    auto k = Kernel::boot(kc);
    if (!k.ok()) {
      err << "fatal: aios-git could not start its kernel at " << kc.root << ": " << k.error().message << "\n";
      return 128;
    }
    c.kernel = std::move(*k);
    auto it = commands().find(c.args[0]);
    if (it == commands().end()) {
      // an alias, from the configuration a repository here (or the global one) gives
      c.try_open_repo();
      if (!c.repo) c.load_config();
      if (auto alias = c.config.get("alias." + c.args[0])) {
        if (!alias->empty() && (*alias)[0] == '!') unsupported(c, "shell aliases ('" + c.args[0] + "' runs a program)");
        std::vector<std::string> expanded;
        std::string cur;
        bool q1 = false, q2 = false;
        for (char ch : *alias) {
          if (ch == '\'' && !q2) { q1 = !q1; continue; }
          if (ch == '"' && !q1) { q2 = !q2; continue; }
          if (ch == ' ' && !q1 && !q2) { if (!cur.empty()) expanded.push_back(cur); cur.clear(); continue; }
          cur += ch;
        }
        if (!cur.empty()) expanded.push_back(cur);
        if (expanded.empty()) die(c, "empty alias for " + c.args[0]);
        for (size_t k2 = 1; k2 < c.args.size(); ++k2) expanded.push_back(c.args[k2]);
        it = commands().find(expanded[0]);
        if (it == commands().end()) die(c, "alias '" + c.args[0] + "' names '" + expanded[0] + "', which is not a git command");
        c.args = expanded;
        c.repo.reset();
      } else {
        err << "git: '" << c.args[0] << "' is not a git command. See 'git --help'.\n";
        return 1;
      }
    }
    // log, show, diff and the like page and color on a terminal; everything else writes straight through
    Terminal terminal(c, tout, out, &out == &std::cout, pager_opt);
    // The change layer takes over commit and stash; the operation log records what every command does.
    const bool hooked = c.args[0] != "init" && c.args[0] != "clone";
    // (the fsmonitor daemon's snapshot takes it only when its lock-free path cannot do: git change snapshot --auto)
    const bool auto_snapshot = c.args.size() > 2 && c.args[0] == "change" && c.args[1] == "snapshot" && c.args[2] == "--auto";
    if (hooked && !auto_snapshot && oplog::needs_lock(c)) {
      // the fsmonitor daemon's background snapshot asks not to wait: a command running now snapshots anyway
      bool background = c.env.count("AIOS_GIT_LOCK_TRY") != 0;
      if (!take_repo_lock(c, !background)) return 75;   // EX_TEMPFAIL: the daemon tries again later
    }
    if (hooked) {
      oplog::before_command(c);
      // The repository is read afresh after what the operation log or the change layer may have written. Where
      // neither is on, nothing was written: the open repository is kept (it was opened, read and dropped twice per
      // command - its config, HEAD and index).
      const bool layered = c.repo && oplog::enabled(c);
      if (layered) { c.repo.reset(); change::forget(c); }
      int taken = change::intercept(c);
      if (taken >= 0) { out.flush(); return taken; }
      if (layered) c.repo.reset();
    }
    int rc;
    // diff_warn_rename_limit: what rename detection gave up on, once the output is out
    auto warn_renames = [&]() {
      const std::string& cmd = c.args.empty() ? std::string() : c.args[0];
      if (cmd != "diff" && cmd != "log" && cmd != "show" && cmd != "whatchanged") return;
      if (!c.degraded_cc_to_c && !c.needed_rename_limit) return;
      out.flush();
      if (c.degraded_cc_to_c) err << "warning: only found copies from modified paths due to too many files.\n";
      else err << "warning: exhaustive rename detection was skipped due to too many files.\n";
      if (c.needed_rename_limit > 0)
        err << "warning: you may want to set your diff.renameLimit variable to at least " << c.needed_rename_limit << " and retry the command.\n";
    };
    try {
      rc = it->second(c);
    } catch (const Exit&) {
      // a command that stopped part way may still have changed something (a merge that left conflicts)
      if (hooked) { try { oplog::after_command(c); } catch (const Exit&) {} }
      throw;
    }
    if (hooked) oplog::after_command(c);
    out.flush();
    warn_renames();
    return rc;
  } catch (const Exit& e) {
    out.flush();
    return e.code;
  } catch (const std::exception& e) {
    out.flush();
    err << "fatal: " << e.what() << "\n";
    return 128;
  }
}

}  // namespace aios_git
