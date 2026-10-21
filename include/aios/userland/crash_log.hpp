#ifndef AIOS_USERLAND_CRASH_LOG_HPP
#define AIOS_USERLAND_CRASH_LOG_HPP

#include <cstddef>

namespace aios::userland {

// Fatal-signal logging for the engine: SIGSEGV, SIGABRT, SIGBUS, SIGILL and SIGFPE each write, to stderr (the engine's
// .err log), the signal, the faulting address, the last SQL statement the crashing thread ran, and a backtrace; then
// the default action is restored and the signal re-raised, so the exit status and the OS crash report are unchanged.
// It runs on its own stack, so a stack overflow is logged too. Only async-signal-safe calls are made in the handler.
// Safe to call more than once.
void install_crash_handler();

// The statement the calling thread is running, kept (truncated to a fixed size) for the handler to print. The thread
// must have called this before it crashes: nothing is read from another thread's state.
void set_last_statement(const char* sql, size_t len);

}  // namespace aios::userland

#endif  // AIOS_USERLAND_CRASH_LOG_HPP
