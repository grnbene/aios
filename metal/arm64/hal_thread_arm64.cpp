/* aios::hal::Thread for ARM64 bare metal - real dispatch to an idle
 * secondary core via metal/arm64/smp.cpp's job inbox (smp_thread_reserve/
 * dispatch/done/reap), the same design metal/smp.cpp already proved for
 * x86's LAPIC/SIPI cores, adapted to PSCI CPU_ON. Falls back to running the
 * body inline on the calling core when no secondary core is free or none
 * ever came online - correct (the caller still gets the work done), just
 * not concurrent for that one call. */
#include "aios/hal/sync.hpp"
#include <functional>
#include <new>

extern "C" int smp_thread_reserve();
extern "C" void smp_thread_dispatch(int slot, void (*fn)(void*), void* ctx);
extern "C" int smp_thread_done(int slot);
extern "C" void smp_thread_reap(int slot);
extern "C" void smp_thread_detach(int slot);
unsigned smp_online_count();  // metal/arm64/smp.cpp
void smp_start();  // metal/arm64/smp.cpp - plain C++ linkage there, not extern "C"

namespace {
volatile bool g_smp_attempted = false;

void run_std_function(void* ctx) {
  auto* f = static_cast<std::function<void()>*>(ctx);
  (*f)();
}
}  // namespace

namespace aios::hal {

struct Thread::State {
  int slot;
  std::function<void()>* job;
};

Thread::Thread(std::function<void()> body) {
  // Secondary cores are only brought up once, on first use - hal::Thread is
  // the first real consumer of them (smp_start() before this was only ever
  // invoked manually, from the "smp" shell command, to prove PSCI bring-up).
  /* No longer brings the secondary cores up by itself. It used to try, and
   * failed (no PSCI on the Pi), so every thread ran inline; now smp_start()
   * works, and flipping every existing hal::Thread user to real concurrency
   * - over mutexes that are still no-ops here - is not something to do by
   * surprise. Cores started explicitly (`smp`, the desktop) are used. */
  (void)g_smp_attempted;

  int slot = smp_thread_reserve();
  if (slot < 0) {
    body();  // no idle core - do the work here rather than drop it
    started_ = false;
    return;
  }
  auto* heap = new std::function<void()>(std::move(body));
  smp_thread_dispatch(slot, run_std_function, heap);
  state_ = new State{slot, heap};
  started_ = true;
}

Thread::~Thread() { if (started_) join(); }

Thread::Thread(Thread&& o) noexcept : state_(o.state_), started_(o.started_) {
  o.state_ = nullptr; o.started_ = false;
}
Thread& Thread::operator=(Thread&& o) noexcept {
  if (this != &o) {
    if (started_) join();
    state_ = o.state_; started_ = o.started_;
    o.state_ = nullptr; o.started_ = false;
  }
  return *this;
}

void Thread::join() {
  if (!started_) return;
  while (!smp_thread_done(state_->slot)) asm volatile("yield");
  smp_thread_reap(state_->slot);
  delete state_->job;
  delete state_;
  state_ = nullptr;
  started_ = false;
}

// The core stays reserved until the body returns; the next reservation that
// finds it done frees it. The body's closure is kept, as nothing waits to delete it.
void Thread::detach() { if (!started_) return; smp_thread_detach(state_->slot); delete state_; state_ = nullptr; started_ = false; }

unsigned hardware_threads() { unsigned n = smp_online_count(); return n ? n : 1u; }

}  // namespace aios::hal
