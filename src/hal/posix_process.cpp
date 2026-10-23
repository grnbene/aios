// Processes on a substrate that has them. Both hosted boundaries (Linux and
// macOS) run the same POSIX code; bare metal answers Unsupported from its own
// implementation, because a machine with one address space has nowhere to put
// a second program.
#include "aios/hal/process.hpp"
#include "aios/hal/sync.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <sys/wait.h>
#include <unistd.h>

#ifndef __linux__
extern char** environ;
#endif

namespace aios::hal {
namespace {

struct Child {
  pid_t pid = -1;
  bool finished = false;
  ProcessStatus last;
};

// Deliberately never destroyed. A supervisor releases the handles it holds
// from its own destructor, and function-local statics are torn down in reverse
// order of INITIALISATION: this table is first touched by spawn_process, which
// is necessarily later than the manager that called it, so a destroyed table
// and mutex would be exactly what that destructor reached for. The leak is one
// map for the life of the process; the alternative was a heap corruption on
// every shutdown that had ever started a service.
Mutex& table_mutex() { static Mutex* m = new Mutex(); return *m; }
std::map<ProcessHandle, Child>& table() {
  static std::map<ProcessHandle, Child>* t = new std::map<ProcessHandle, Child>();
  return *t;
}
ProcessHandle next_handle() { static ProcessHandle n = 1; return n++; }

int signal_number(ProcessSignal s) {
  switch (s) {
    case ProcessSignal::Interrupt: return SIGINT;
    case ProcessSignal::Terminate: return SIGTERM;
    case ProcessSignal::Kill:      return SIGKILL;
    case ProcessSignal::Hangup:    return SIGHUP;
  }
  return SIGTERM;
}

// Collects the child if it has finished. The caller holds the table lock.
void refresh(Child& c) {
  if (c.finished || c.pid <= 0) return;
  int st = 0;
  pid_t r = ::waitpid(c.pid, &st, WNOHANG);
  if (r == 0) { c.last.running = true; return; }
  if (r < 0 && errno == ECHILD) {
    // Somebody else reaped it; all we can honestly say is that it is gone.
    c.finished = true;
    c.last = ProcessStatus{false, true, 0, 0};
    return;
  }
  if (r == c.pid) {
    c.finished = true;
    c.last.running = false;
    c.last.exited = true;
    c.last.exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 0;
    c.last.terminating_signal = WIFSIGNALED(st) ? WTERMSIG(st) : 0;
  }
}

}  // namespace

Result<ProcessHandle> spawn_process(const ProcessSpawn& spec) {
  if (spec.argv.empty()) return Result<ProcessHandle>(Status::Invalid, "a process needs a program to run");

  std::vector<char*> argv;
  argv.reserve(spec.argv.size() + 1);
  for (const auto& s : spec.argv) argv.push_back(const_cast<char*>(s.c_str()));
  argv.push_back(nullptr);

  std::vector<std::string> env_pairs;
  for (const auto& [k, v] : spec.env) env_pairs.push_back(k + "=" + v);

  pid_t pid = ::fork();
  if (pid < 0) return Result<ProcessHandle>(Status::IoError, std::string("fork: ") + std::strerror(errno));

  if (pid == 0) {
    // A supervised service must survive the console that started it and must
    // not read from it, so it gets its own session and a closed input.
    ::setsid();
    int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) { ::dup2(devnull, 0); if (devnull > 2) ::close(devnull); }
    int out = spec.output_path.empty()
                  ? ::open("/dev/null", O_WRONLY)
                  : ::open(spec.output_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (out >= 0) { ::dup2(out, 1); ::dup2(out, 2); if (out > 2) ::close(out); }
    if (!spec.working_dir.empty() && ::chdir(spec.working_dir.c_str()) != 0) ::_exit(126);
    if (spec.clear_env) {
#ifdef __linux__
      ::clearenv();
#else
      static char* empty_env[] = {nullptr};
      environ = empty_env;
#endif
    }
    for (const auto& n : spec.unset_env) ::unsetenv(n.c_str());
    for (const auto& e : env_pairs) ::putenv(const_cast<char*>(e.c_str()));
    ::execvp(argv[0], argv.data());
    ::_exit(127);
  }

  LockGuard l(table_mutex());
  ProcessHandle h = next_handle();
  Child c;
  c.pid = pid;
  c.last.running = true;
  table()[h] = c;
  return h;
}

Result<ProcessStatus> process_status(ProcessHandle h) {
  LockGuard l(table_mutex());
  auto it = table().find(h);
  if (it == table().end()) return Result<ProcessStatus>(Status::NotFound, "no such process handle");
  refresh(it->second);
  return it->second.last;
}

Result<ProcessStatus> wait_process(ProcessHandle h, uint32_t timeout_ms) {
  // Polling rather than a blocking waitpid: the table is shared with a
  // supervisor that may be asking about other children at the same time, and
  // holding the lock across an indefinite wait would stall it.
  for (uint32_t waited = 0;; waited += 10) {
    {
      LockGuard l(table_mutex());
      auto it = table().find(h);
      if (it == table().end()) return Result<ProcessStatus>(Status::NotFound, "no such process handle");
      refresh(it->second);
      if (it->second.finished) return it->second.last;
    }
    if (waited >= timeout_ms) return Result<ProcessStatus>(Status::Busy, "the process is still running");
    sleep_ms(10);
  }
}

Result<void> signal_process(ProcessHandle h, ProcessSignal sig) {
  LockGuard l(table_mutex());
  auto it = table().find(h);
  if (it == table().end()) return Result<void>(Status::NotFound, "no such process handle");
  refresh(it->second);
  if (it->second.finished) return Result<void>(Status::Invalid, "the process has already finished");
  // The child was given its own session, so the whole group goes with it.
  if (::kill(-it->second.pid, signal_number(sig)) != 0 && errno == ESRCH) {
    if (::kill(it->second.pid, signal_number(sig)) != 0) {
      return Result<void>(Status::IoError, std::string("kill: ") + std::strerror(errno));
    }
  }
  return Ok();
}

std::string find_program(const std::vector<std::string>& names) {
  const char* path = ::getenv("PATH");
  std::string search = path ? path : "/usr/local/bin:/usr/bin:/bin:/opt/homebrew/bin";
  for (const auto& name : names) {
    if (name.find('/') != std::string::npos) {
      if (::access(name.c_str(), X_OK) == 0) return name;
      continue;
    }
    size_t at = 0;
    while (at <= search.size()) {
      size_t sep = search.find(':', at);
      if (sep == std::string::npos) sep = search.size();
      std::string dir = search.substr(at, sep - at);
      at = sep + 1;
      if (dir.empty()) continue;
      std::string full = dir + "/" + name;
      if (::access(full.c_str(), X_OK) == 0) return full;
    }
  }
  return {};
}

Result<std::string> scratch_path(const std::string& suffix) {
  const char* tmp = ::getenv("TMPDIR");
  std::string dir = tmp && *tmp ? tmp : "/tmp";
  if (dir.back() == '/') dir.pop_back();
  std::string tmpl = dir + "/aios-XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  int fd = ::mkstemp(buf.data());
  if (fd < 0) return Result<std::string>(Status::IoError, std::string("mkstemp: ") + std::strerror(errno));
  ::close(fd);
  std::string path(buf.data());
  // mkstemp had to create the file to guarantee the name; a tool that wants to
  // write it fresh, or wants a particular extension, gets a clean path.
  if (!suffix.empty()) {
    ::unlink(path.c_str());
    path += suffix;
  }
  return path;
}

Result<ToolResult> run_tool(const std::vector<std::string>& argv, uint32_t timeout_ms,
                            const std::string& working_dir) {
  ProcessSpawn spec;
  spec.argv = argv;
  spec.working_dir = working_dir;
  return run_tool(spec, timeout_ms);
}

Result<ToolResult> run_tool(const ProcessSpawn& in, uint32_t timeout_ms) {
  if (in.argv.empty()) return Result<ToolResult>(Status::Invalid, "nothing to run: no program was named");
  const auto& argv = in.argv;
  auto out_path = scratch_path(".out");
  if (!out_path.ok()) return out_path.error();

  ProcessSpawn spec = in;
  spec.output_path = *out_path;
  auto h = spawn_process(spec);
  if (!h.ok()) { ::unlink(out_path->c_str()); return h.error(); }

  auto st = wait_process(*h, timeout_ms);
  if (!st.ok()) {
    (void)signal_process(*h, ProcessSignal::Kill);
    (void)release_process(*h);
    ::unlink(out_path->c_str());
    return Result<ToolResult>(Status::Busy, argv[0] + " did not finish within " + std::to_string(timeout_ms) + "ms");
  }
  (void)release_process(*h);

  ToolResult r;
  r.exit_code = st->exit_code;
  if (FILE* f = ::fopen(out_path->c_str(), "rb")) {
    char chunk[4096];
    size_t n;
    while ((n = ::fread(chunk, 1, sizeof chunk, f)) > 0) r.output.append(chunk, n);
    ::fclose(f);
  }
  ::unlink(out_path->c_str());
  return r;
}

Result<void> release_process(ProcessHandle h) {
  LockGuard l(table_mutex());
  auto it = table().find(h);
  if (it == table().end()) return Result<void>(Status::NotFound, "no such process handle");
  refresh(it->second);
  if (!it->second.finished && it->second.pid > 0) {
    ::kill(-it->second.pid, SIGKILL);
    ::kill(it->second.pid, SIGKILL);
    int st = 0;
    ::waitpid(it->second.pid, &st, 0);
  }
  table().erase(it);
  return Ok();
}

}  // namespace aios::hal
