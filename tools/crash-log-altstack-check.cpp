// A thread that records a statement for the crash log (set_last_statement) gets a 64 KB alternate signal stack. When
// the thread ends, that stack must go with it.
//
// The MySQL wire runs a thread per connection, and every statement off the fast table-lookup path records itself.
// Kept "for the life of the thread" but never freed, the stacks outlived their threads: st's engine, polled by a
// monitor that connects for each statement (6.5 a second), grew 1.5 GB an hour and was killed at its 12 GB cap
// three times in a day.

#include "aios/userland/crash_log.hpp"

#include <malloc.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

static size_t live_bytes() {
  struct mallinfo2 m = mallinfo2();
  return m.uordblks + m.hblkhd;
}

int main() {
  aios::userland::install_crash_handler();
  const char* sql = "SHOW GLOBAL STATUS";
  // one thread first: whatever the first use of a thread sets up once for the process is not counted
  std::thread([&] { aios::userland::set_last_statement(sql, std::strlen(sql)); }).join();

  // while the thread runs, the handler has its stack: sigaltstack reports one, enabled
  bool armed = false;
  std::thread([&] {
    aios::userland::set_last_statement(sql, std::strlen(sql));
    stack_t now{};
    armed = sigaltstack(nullptr, &now) == 0 && !(now.ss_flags & SS_DISABLE) && now.ss_sp != nullptr && now.ss_size >= 64 * 1024;
  }).join();
  std::printf("%s a thread that recorded a statement has an alternate signal stack for the crash handler\n", armed ? "ok  " : "FAIL");
  if (!armed) return 1;

  constexpr int kThreads = 200;
  const size_t before = live_bytes();
  for (int i = 0; i < kThreads; ++i) {
    std::thread([&] {
      aios::userland::set_last_statement(sql, std::strlen(sql));
      aios::userland::set_last_statement(sql, std::strlen(sql));   // a second statement on the same thread: no second stack
    }).join();
  }
  const size_t after = live_bytes();
  const long grew = (long)after - (long)before;
  const long per = grew / kThreads;
  // a few hundred bytes a thread of allocator noise is not a leak; 64 KB a thread is
  const bool ok = per < 1024;
  std::printf("%s %d threads that recorded a statement and ended: %ld bytes still allocated (%ld a thread)\n",
              ok ? "ok  " : "FAIL", kThreads, grew, per);
  return ok ? 0 : 1;
}
