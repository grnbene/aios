#undef NDEBUG
// The engine's crash log: a real crash in a forked child, its stderr captured, and what the log says checked.
//  [1] a null write: SIGSEGV is logged with the address, the statement the thread last ran and a backtrace, and the
//      child still dies by SIGSEGV (the default action is restored and re-raised);
//  [2] the statement really comes from the SQL engine: a statement run through SqlDatabase, then a crash;
//  [3] a stack overflow is logged too (the handler runs on its own stack);
//  [4] SIGABRT, with no statement recorded on the thread.
#include <cerrno>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include "aios/kernel/kernel.hpp"
#include "aios/storage/aql.hpp"
#include "aios/userland/crash_log.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/task_ledger.hpp"

using namespace aios;
using namespace aios::userland;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& label) {
  std::cout << (ok ? "  [ok] " : "  [FAIL] ") << label << "\n";
  if (!ok) g_failures++;
}

struct Child {
  std::string err;   // what it wrote to stderr
  int status = 0;
};

// Forks, runs `body` in the child with stderr into a pipe, and returns what it wrote and how it ended.
template <class F>
Child run_child(F body) {
  int fd[2];
  if (pipe(fd) != 0) { std::perror("pipe"); std::exit(2); }
  std::cout.flush();   // so the child does not inherit (and abort() then print) a copy of what is buffered
  pid_t pid = fork();
  if (pid < 0) { std::perror("fork"); std::exit(2); }
  if (pid == 0) {
    close(fd[0]);
    dup2(fd[1], STDERR_FILENO);
    close(fd[1]);
    body();
    _exit(0);   // the body was meant to crash
  }
  close(fd[1]);
  Child c;
  char buf[4096];
  for (;;) {
    ssize_t n = read(fd[0], buf, sizeof buf);
    if (n > 0) c.err.append(buf, (size_t)n);
    else if (n == 0) break;
    else if (errno != EINTR) break;
  }
  close(fd[0]);
  waitpid(pid, &c.status, 0);
  return c;
}

bool died_by(const Child& c, int sig) { return WIFSIGNALED(c.status) && WTERMSIG(c.status) == sig; }
bool has(const Child& c, const std::string& s) { return c.err.find(s) != std::string::npos; }

// A backtrace frame: a line after "Backtrace:" with an address in it (both glibc's and macOS's formats have one).
bool has_frame(const Child& c) {
  const size_t at = c.err.find("Backtrace:\n");
  if (at == std::string::npos) return false;
  size_t pos = at + 11;
  while (pos < c.err.size()) {
    size_t eol = c.err.find('\n', pos);
    if (eol == std::string::npos) eol = c.err.size();
    const std::string line = c.err.substr(pos, eol - pos);
    if (line.rfind("***", 0) == 0) return false;
    if (line.find("0x") != std::string::npos) return true;
    pos = eol + 1;
  }
  return false;
}

[[gnu::noinline]] void write_null() {
#if !defined(_MSC_VER)
  volatile int* p = nullptr;
  asm volatile("" : "+r"(p));
  *p = 42;
#else
  *reinterpret_cast<volatile int*>(0) = 42;
#endif
}

// Runs the stack out: 4 KB a frame, never returning before the stack is gone (the depth check is never reached,
// and keeps the recursion from being one the compiler can see is endless).
[[gnu::noinline]] int recurse(int depth) {
  volatile char pad[4096];
  pad[0] = (char)depth;
  if (depth == 1 << 30) return 0;
  return recurse(depth + 1) + pad[0];
}

}  // namespace

int main() {
  std::cout << "=== asql crash log ===\n";

  std::cout << "\n[1] a null write\n";
  {
    const std::string stmt = "SELECT 'statement-before-the-null-write'";
    Child c = run_child([&] {
      install_crash_handler();
      set_last_statement(stmt.data(), stmt.size());
      write_null();
    });
    check(died_by(c, SIGSEGV), "the child still dies by SIGSEGV");
    check(has(c, "SIGSEGV"), "the log names SIGSEGV");
    check(has(c, "at address 0x0"), "and the faulting address");
    check(has(c, "Last statement: " + stmt), "and the statement the thread last ran");
    check(has_frame(c), "and at least one backtrace frame");
    check(has(c, "*** end of crash log ***"), "and the log is complete");
  }

  std::cout << "\n[2] the statement comes from the SQL engine\n";
  {
    char root_dir[] = "/tmp/aios-crashlog-XXXXXX";
    if (!mkdtemp(root_dir)) { std::perror("mkdtemp"); return 2; }
    const std::string root = root_dir;
    Child c = run_child([&] {
      KernelConfig cfg;
      cfg.root = root;
      cfg.residency = test_config;
      auto k_res = Kernel::boot(cfg);
      if (!k_res.ok()) _exit(3);
      Kernel& kernel = *k_res.value();
      TaskLedger ledger(kernel);
      ledger.init("tasks");
      SqlDatabase db(kernel, &ledger);
      db.init("sql_db");
      install_crash_handler();
      db.execute_gateway_query("SELECT 41 + 1 AS the_answer_before_the_crash;");
      write_null();
    });
    check(died_by(c, SIGSEGV), "the child dies by SIGSEGV");
    check(has(c, "Last statement: SELECT 41 + 1 AS the_answer_before_the_crash;"), "the log has the statement run through SqlDatabase");
  }

  std::cout << "\n[3] a stack overflow\n";
  {
    Child c = run_child([&] {
      install_crash_handler();
      const std::string stmt = "SELECT 'statement-before-the-overflow'";
      set_last_statement(stmt.data(), stmt.size());
      recurse(0);
    });
    check(died_by(c, SIGSEGV) || died_by(c, SIGBUS), "the child dies by SIGSEGV or SIGBUS");
    check(has(c, "SIGSEGV") || has(c, "SIGBUS"), "the overflow is logged (the handler ran on its own stack)");
    check(has(c, "Last statement: SELECT 'statement-before-the-overflow'"), "with the statement");
  }

  std::cout << "\n[4] SIGABRT, no statement recorded\n";
  {
    Child c = run_child([&] {
      install_crash_handler();
      abort();
    });
    check(died_by(c, SIGABRT), "the child dies by SIGABRT");
    check(has(c, "SIGABRT"), "the log names SIGABRT");
    check(has(c, "(none recorded on this thread)"), "and says no statement was recorded");
  }

  std::cout << "\n=== " << (g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES: " + std::to_string(g_failures)) << " ===\n";
  return g_failures == 0 ? 0 : 1;
}
