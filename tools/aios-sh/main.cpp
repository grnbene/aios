// aios-sh: the entry point. sh's own command line (-c, a script, standard input, -e -u -x -o opt), the
// environment imported as exported variables, and a plain interactive prompt for a person at a console.
#include "sh.hpp"

#include <csignal>
#include <cstring>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#elif defined(__APPLE__)
#include <crt_externs.h>
#else
extern char** environ;   // at global scope: declared inside the namespace it would be a different, undefined symbol
#endif

namespace aiossh {

namespace {

const char* kUsage =
    "usage: aios-sh [-euxv] [-o option] [-c command [name [arg ...]] | script [arg ...]]\n"
    "  The POSIX shell and utilities of aios. Host programs on PATH run as themselves.\n";

void import_environment(State& st) {
#ifdef _WIN32
  if (LPWCH block = ::GetEnvironmentStringsW()) {
    for (LPWCH p = block; *p;) {
      std::wstring kv(p);
      p += kv.size() + 1;
      int n = ::WideCharToMultiByte(CP_UTF8, 0, kv.data(), static_cast<int>(kv.size()), nullptr, 0, nullptr, nullptr);
      std::string s(static_cast<size_t>(n), '\0');
      ::WideCharToMultiByte(CP_UTF8, 0, kv.data(), static_cast<int>(kv.size()), s.data(), n, nullptr, nullptr);
      size_t eq = s.find('=', 1);
      if (eq == std::string::npos || eq == 0) continue;
      std::string k = s.substr(0, eq);
      std::string up = k;
      for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      if (up == "PATH") k = "PATH";   // Windows spells it Path; scripts say PATH
      st.vars[k] = Var{s.substr(eq + 1), true, false};
    }
    ::FreeEnvironmentStringsW(block);
  }
  if (!st.has("HOME")) {
    std::string h = os::home_dir();
    for (auto& c : h) if (c == '\\') c = '/';
    st.vars["HOME"] = Var{h, true, false};
  }
#elif defined(__APPLE__)
  char** env = *_NSGetEnviron();
  for (char** e = env; e && *e; ++e) {
    std::string s = *e;
    size_t eq = s.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    st.vars[s.substr(0, eq)] = Var{s.substr(eq + 1), true, false};
  }
#else
  for (char** e = ::environ; e && *e; ++e) {
    std::string s = *e;
    size_t eq = s.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    st.vars[s.substr(0, eq)] = Var{s.substr(eq + 1), true, false};
  }
#endif
  st.vars.erase("PWD");
  if (!st.has("IFS")) st.vars["IFS"] = Var{" \t\n", false, false};
  if (!st.has("PS1")) st.vars["PS1"] = Var{"$ ", false, false};
}

std::vector<std::string> utf8_args(int argc, char** argv) {
  std::vector<std::string> out;
#ifdef _WIN32
  int n = 0;
  LPWSTR* w = ::CommandLineToArgvW(::GetCommandLineW(), &n);
  if (w) {
    for (int i = 0; i < n; ++i) {
      int len = ::WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
      std::string s(static_cast<size_t>(len > 0 ? len - 1 : 0), '\0');
      if (len > 1) ::WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
      out.push_back(s);
    }
    ::LocalFree(w);
    return out;
  }
#endif
  for (int i = 0; i < argc; ++i) out.push_back(argv[i]);
  return out;
}

// Applies sh's options from argv[start..]; returns the index of the first operand. Sets *command when -c.
size_t shell_options(State& st, const std::vector<std::string>& argv, size_t start, bool* dash_c, bool* dash_s) {
  size_t i = start;
  for (; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (a == "--") { ++i; break; }
    if (a == "--login" || a == "--norc" || a == "--noprofile" || a == "--posix") continue;
    if (a.size() < 2 || (a[0] != '-' && a[0] != '+')) break;
    bool on = a[0] == '-';
    for (size_t k = 1; k < a.size(); ++k) {
      char c = a[k];
      if (c == 'c') *dash_c = true;
      else if (c == 's') *dash_s = true;
      else if (c == 'e') st.opt_errexit = on;
      else if (c == 'u') st.opt_nounset = on;
      else if (c == 'x') st.opt_xtrace = on;
      else if (c == 'f') st.opt_noglob = on;
      else if (c == 'n') st.opt_noexec = on;
      else if (c == 'o') {
        if (i + 1 < argv.size()) {
          std::string o = argv[++i];
          if (o == "pipefail") st.opt_pipefail = on; else if (o == "errexit") st.opt_errexit = on;
          else if (o == "nounset") st.opt_nounset = on; else if (o == "xtrace") st.opt_xtrace = on; else if (o == "noglob") st.opt_noglob = on;
          else if (o == "noexec") st.opt_noexec = on;
        }
        break;
      }
      // -i -l -v -h -m -a -b -C: accepted
    }
  }
  return i;
}

int run_guarded(Shell& sh, State& st, const std::string& text, const Io& io) {
  try {
    return sh.run_text(st, text, io);
  } catch (const ExitSignal& e) {
    return e.status;
  } catch (const ReturnSignal& r) {
    return r.status;
  } catch (const LoopSignal&) {
    return 0;
  }
}

int interactive(Shell& sh, State& st, const Io& io) {
  std::string pending;
  LineReader r(io.in);
  for (;;) {
    io.ewrite(pending.empty() ? st.get("PS1") : "> ");
    std::string line;
    if (!r.next(line)) { io.ewrite("\n"); break; }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    pending += line + "\n";
    bool incomplete = false;
    try {
      auto tree = parse(pending, &incomplete);
      if (incomplete) continue;
      try { st.last_status = sh.run(st, *tree, io); }
      catch (const ExitSignal& e) { return e.status; }
      catch (const ReturnSignal&) {}
      catch (const LoopSignal&) {}
    } catch (const ParseError& e) {
      sh.error(io, "syntax error: " + e.message);
      st.last_status = 2;
    }
    pending.clear();
  }
  return st.last_status;
}

}  // namespace

// `sh ...` / `bash ...` run from inside the shell: a subshell of this one, never a host shell.
int shell_main_like(Shell& sh, State& st, const std::vector<std::string>& argv, const Io& io) {
  State sub = st;
  bool dash_c = false, dash_s = false;
  size_t i = shell_options(sub, argv, 1, &dash_c, &dash_s);
  if (dash_c) {
    if (i >= argv.size()) { sh.error(io, argv[0] + ": -c: option requires an argument"); return 2; }
    std::string text = argv[i++];
    if (i < argv.size()) sub.arg0 = argv[i++];
    sub.params.assign(argv.begin() + static_cast<long>(i), argv.end());
    return run_guarded(sh, sub, text, io);
  }
  if (i < argv.size() && !dash_s) {
    std::string path = host_path(sub.cwd, argv[i]), text, e;
    if (!os::read_whole(path, text, &e)) {
      std::string found = os::find_program(argv[i], sub.get("PATH"), sub.cwd);
      if (found.empty() || !os::read_whole(found, text, &e)) { sh.error(io, argv[i] + ": " + e); return 127; }
    }
    sub.arg0 = argv[i];
    sub.params.assign(argv.begin() + static_cast<long>(i) + 1, argv.end());
    return run_guarded(sh, sub, text, io);
  }
  sub.params.assign(argv.begin() + static_cast<long>(i), argv.end());
  return run_guarded(sh, sub, io.read_all(), io);
}

}  // namespace aiossh

// The shell proper. aios-sh's main() below and `aios sh` (cli/main.cpp) both call this; argv[0] is
// the program name, the rest is the shell's own command line.
int aios_sh_main(int argc, char** argv) {
  using namespace aiossh;
  std::vector<std::string> args = utf8_args(argc, argv);
  if (args.size() > 1 && (args[1] == "--help" || args[1] == "-h")) { std::fputs(kUsage, stdout); return 0; }
  if (args.size() > 1 && args[1] == "--version") { std::fputs("aios-sh 1.0 (aios)\n", stdout); return 0; }
#ifndef _WIN32
  // A write to a pipe whose reader has gone fails with EPIPE and stops that stage (BrokenPipe in sh_exec.cpp)
  // instead of killing the whole shell; host programs get SIGPIPE back in spawn().
  ::signal(SIGPIPE, SIG_IGN);
#endif
  Shell sh;
  struct TempCleanup {
    Shell& sh;
    ~TempCleanup() { std::string e; for (const auto& p : sh.temp_files) os::remove_file(p, &e); }
  } cleanup{sh};
  sh.self = os::self_exe();
  State st;
  import_environment(st);
  st.cwd = os::current_dir();
#ifndef AIOS_SH_NO_MAIN
  st.vars["SHELL"] = Var{sh.self, true, false};   // aios-sh is a program of its own; inside `aios sh` the host's SHELL stands
#endif
  Io io{os::std_fd(0), os::std_fd(1), os::std_fd(2)};
  bool dash_c = false, dash_s = false;
  size_t i = shell_options(st, args, 1, &dash_c, &dash_s);
  if (dash_c) {
    if (i >= args.size()) { std::fputs("aios-sh: -c: option requires an argument\n", stderr); return 2; }
    std::string text = args[i++];
    if (i < args.size()) st.arg0 = args[i++];
    st.params.assign(args.begin() + static_cast<long>(i), args.end());
    return run_guarded(sh, st, text, io);
  }
  if (i < args.size() && !dash_s) {
    std::string text, e;
    std::string path = host_path(st.cwd, args[i]);
    if (!os::read_whole(path, text, &e)) { std::fprintf(stderr, "aios-sh: %s: %s\n", args[i].c_str(), e.c_str()); return 127; }
    st.arg0 = args[i];
    st.params.assign(args.begin() + static_cast<long>(i) + 1, args.end());
    return run_guarded(sh, st, text, io);
  }
  st.params.assign(args.begin() + static_cast<long>(i), args.end());
  if (os::is_console(io.in)) return interactive(sh, st, io);
  return run_guarded(sh, st, io.read_all(), io);
}

#ifndef AIOS_SH_NO_MAIN
int main(int argc, char** argv) { return aios_sh_main(argc, argv); }
#endif
