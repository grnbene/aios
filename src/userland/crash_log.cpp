#include "aios/userland/crash_log.hpp"

#include <csignal>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>

#include <execinfo.h>
#include <pthread.h>
#include <unistd.h>

namespace aios::userland {

namespace {

constexpr size_t kStatementMax = 512;
constexpr size_t kAltStackSize = 64 * 1024;

// Plain arrays with constant initialisation: reading them in the handler needs no constructor, no allocation.
thread_local char tls_statement[kStatementMax];
thread_local size_t tls_statement_len = 0;

// One alternate stack per thread, made the first time the thread records a statement (or installs the handler), so a
// thread that overflows its stack still has somewhere to run the handler.
thread_local bool tls_altstack_done = false;
void ensure_altstack() {
  if (tls_altstack_done) return;
  tls_altstack_done = true;
  // MINSIGSTKSZ is a sysconf() call on glibc 2.34+, not a constant: checked here.
  if (kAltStackSize < (size_t)MINSIGSTKSZ) return;
  void* mem = ::operator new(kAltStackSize, std::nothrow);   // kept for the life of the thread
  if (!mem) return;
  stack_t ss{};
  ss.ss_sp = mem;
  ss.ss_size = kAltStackSize;
  if (sigaltstack(&ss, nullptr) != 0) ::operator delete(mem);
}

void put(const char* s, size_t n) {
  while (n > 0) {
    ssize_t w = ::write(STDERR_FILENO, s, n);
    if (w <= 0) return;
    s += w;
    n -= (size_t)w;
  }
}
void put(const char* s) { put(s, std::strlen(s)); }

// "0x" + hex, into buf (at least 19 bytes); returns the length.
size_t hex(uintptr_t v, char* buf) {
  static const char d[] = "0123456789abcdef";
  char tmp[16];
  size_t n = 0;
  do { tmp[n++] = d[v & 0xf]; v >>= 4; } while (v);
  size_t o = 0;
  buf[o++] = '0';
  buf[o++] = 'x';
  while (n) buf[o++] = tmp[--n];
  return o;
}
size_t decimal(long v, char* buf) {
  char tmp[24];
  size_t n = 0;
  const bool neg = v < 0;
  unsigned long u = neg ? 0UL - (unsigned long)v : (unsigned long)v;
  do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
  size_t o = 0;
  if (neg) buf[o++] = '-';
  while (n) buf[o++] = tmp[--n];
  return o;
}

const char* signal_name(int sig) {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    default: return "signal";
  }
}

void on_fatal_signal(int sig, siginfo_t* info, void*) {
  char num[32];
  put("\n*** aios fatal signal: ");
  put(signal_name(sig));
  put(" (");
  put(num, decimal(sig, num));
  put(")");
  if (info && (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE)) {
    put(" at address ");
    put(num, hex((uintptr_t)info->si_addr, num));
  }
  put(" in pid ");
  put(num, decimal((long)getpid(), num));
  put(" ***\n");
  put("Last statement: ");
  if (tls_statement_len) put(tls_statement, tls_statement_len);
  else put("(none recorded on this thread)");
  put("\nBacktrace:\n");
  void* frames[64];
  const int n = backtrace(frames, 64);
  backtrace_symbols_fd(frames, n, STDERR_FILENO);
  put("*** end of crash log ***\n");

  // Back to the default action, signal unblocked, and raised again: the process dies as it would have, with the
  // OS's own crash report.
  struct sigaction dfl;
  std::memset(&dfl, 0, sizeof dfl);
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  sigaction(sig, &dfl, nullptr);
  sigset_t unblock;
  sigemptyset(&unblock);
  sigaddset(&unblock, sig);
  pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
  raise(sig);
}

}  // namespace

void set_last_statement(const char* sql, size_t len) {
  ensure_altstack();
  if (!sql) len = 0;
  if (len > kStatementMax) len = kStatementMax;
  if (len) std::memcpy(tls_statement, sql, len);
  tls_statement_len = len;
}

void install_crash_handler() {
  static std::once_flag once;
  std::call_once(once, [] {
    // backtrace() may allocate and load the unwinder on its first call: do that now, not inside a crashed process.
    void* warm[4];
    backtrace(warm, 4);
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fatal_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE}) sigaction(sig, &sa, nullptr);
  });
  ensure_altstack();   // the installing thread (the main one) too
}

}  // namespace aios::userland
