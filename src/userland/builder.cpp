// This runs `cmake --build` and `scripts/drive.sh` as child processes, which a
// freestanding kernel has no way to provide -- and metal/build.sh compiles every src/*.cpp, so the
// bare-metal image stopped linking on the host process calls the moment
// this file arrived. Nothing calls BuilderManager yet; on metal it is simply
// not built. The console's own `build` command does not come through here.
#if !defined(AIOS_METAL)
#include "aios/userland/builder.hpp"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include "aios/userland/git.hpp"
#include "aios/hal/host.hpp"
#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#else
#include <windows.h>
#endif

namespace aios::userland {

namespace {
#if defined(_WIN32)
// One argv entry as the Windows command line the C runtime parses back into the same entry: quoted when it holds
// a space or quote, with backslashes before a quote doubled. No shell is involved either way.
std::string win_quote_arg(const std::string& a) {
  if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) return a;
  std::string out = "\"";
  size_t slashes = 0;
  for (char ch : a) {
    if (ch == '\\') { ++slashes; continue; }
    if (ch == '"') { out.append(slashes * 2 + 1, '\\'); out += '"'; }
    else { out.append(slashes, '\\'); out += ch; }
    slashes = 0;
  }
  out.append(slashes * 2, '\\');
  out += '"';
  return out;
}

// Run args[0] with args, no shell; stdout+stderr into `out`. Exit status, or -1.
int run_argv(const std::vector<std::string>& args, std::string& out) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  std::string cmd;
  for (const auto& a : args) { if (!cmd.empty()) cmd += ' '; cmd += win_quote_arg(a); }
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nul;
  si.hStdOutput = wr;
  si.hStdError = wr;
  PROCESS_INFORMATION pi{};
  const BOOL started = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(wr);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (!started) { CloseHandle(rd); out += "cannot start " + args[0] + "\n"; return -1; }
  char buf[512];
  DWORD n = 0;
  while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, (size_t)n);
  CloseHandle(rd);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return (int)code;
}
#else
// Run argv[0] with argv, no shell; stdout+stderr into `out`. Exit status, or -1.
int run_argv(const std::vector<std::string>& args, std::string& out) {
  int fds[2];
  if (::pipe(fds) != 0) return -1;
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
  posix_spawn_file_actions_addclose(&fa, fds[0]);
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = -1;
  int rc = ::posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  ::close(fds[1]);
  if (rc != 0) { ::close(fds[0]); out += "cannot start " + args[0] + "\n"; return -1; }
  char buf[512];
  ssize_t n;
  while ((n = ::read(fds[0], buf, sizeof(buf))) > 0 || (n < 0 && errno == EINTR))
    if (n > 0) out.append(buf, (size_t)n);
  ::close(fds[0]);
  int st = 0;
  while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
#endif
}  // namespace


BuilderManager::BuilderManager(Kernel& kernel) : kernel_(kernel) {}

BuildResult BuilderManager::run_build(const std::string& target) {
  BuildResult res;
  auto t0 = std::chrono::steady_clock::now();
  // No shell: the target is one argv entry, whatever characters it holds.
  std::vector<std::string> args = {"cmake", "--build", "build", "-j4"};
  if (!target.empty()) { args.push_back("--target"); args.push_back(target); }
  std::string out;
  int rc = run_argv(args, out);
  res.exit_code = rc;
  res.success = (rc == 0);
  res.output = out;
  auto t1 = std::chrono::steady_clock::now();
  res.build_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
  if (!res.success) res.errors = res.output;
  return res;
}

TestResult BuilderManager::run_tests() {
  TestResult res;
  std::string out;
  int rc = run_argv({"scripts/drive.sh", "build"}, out);
  // The verdict is drive.sh's own: "<script>: N commands, M unexpected failure(s)",
  // and its exit code. Nothing is assumed when that line is missing.
  long total = -1, failed = -1;
  std::istringstream in(out);
  std::string line;
  while (std::getline(in, line)) {
    const size_t c = line.find(" commands, ");
    const size_t u = line.find(" unexpected failure");
    if (c != std::string::npos && u != std::string::npos) {
      size_t b = line.rfind(' ', c - 1);
      total = std::atol(line.substr(b == std::string::npos ? 0 : b + 1, c).c_str());
      failed = std::atol(line.substr(c + 11, u - (c + 11)).c_str());
    }
    if (line.find("FAILED") != std::string::npos) res.failed_test_names.push_back(line);
  }
  if (total < 0) {
    res.success = false;
    res.summary = "drive.sh printed no result line (exit code " + std::to_string(rc) + ")";
    return res;
  }
  res.total_tests = (int)total;
  res.failed_tests = (int)failed;
  res.passed_tests = (int)(total - failed);
  res.success = (rc == 0 && failed == 0);
  res.summary = "drive.sh: " + std::to_string(res.passed_tests) + "/" + std::to_string(res.total_tests) +
                " passed, exit code " + std::to_string(rc);
  return res;
}

BuildResult BuilderManager::auto_remediate_and_commit(const std::string& commit_msg) {
  BuildResult b_res = run_build();
  if (!b_res.success) {
    b_res.errors += "\n[Auto-Remediate] Build failed. Stopping commit loop.";
    return b_res;
  }

  TestResult t_res = run_tests();
  if (!t_res.success) {
    b_res.success = false;
    b_res.errors = "[Auto-Remediate] Test suite failed: " + t_res.summary;
    return b_res;
  }

  // Build and tests passed cleanly: commit. This used to paste the message
  // inside "..." on a popen() line, so a message with a quote ran commands.
  BuildResult c_res = commit_all(commit_msg);
  b_res.output += "\n[Git Commit] " + c_res.output;
  b_res.success = c_res.success;
  if (!c_res.success) b_res.errors = c_res.errors;
  return b_res;
}


BuildResult BuilderManager::commit_all(const std::string& commit_msg, const std::string& repo) {
  BuildResult r;
  std::string out;
  int rc = run_argv({"git", "-C", repo, "add", "-A"}, out);
  if (rc == 0) rc = run_argv({"git", "-C", repo, "commit", "-m", commit_msg}, out);
  r.exit_code = rc;
  r.success = rc == 0;
  r.output = out;
  if (!r.success) r.errors = out;
  return r;
}

}  // namespace aios::userland
#endif  // !AIOS_METAL
