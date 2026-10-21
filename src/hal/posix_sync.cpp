// Threads and locks on a POSIX host: the same file serves every hosted
// substrate (Linux, macOS). Bare metal implements these against its own
// scheduler instead; nothing above hal/ can tell which one it got.
#include "aios/hal/sync.hpp"
#include <pthread.h>
#include <sys/mman.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <atomic>
#include <new>

namespace aios::hal {

static_assert(sizeof(pthread_mutex_t) <= 64, "mutex storage");

static pthread_mutex_t* pm(unsigned char* s) { return reinterpret_cast<pthread_mutex_t*>(s); }

Mutex::Mutex() { new (storage_) pthread_mutex_t; pthread_mutex_init(pm(storage_), nullptr); }
Mutex::~Mutex() { pthread_mutex_destroy(pm(storage_)); }
void Mutex::lock() { pthread_mutex_lock(pm(storage_)); }
void Mutex::unlock() { pthread_mutex_unlock(pm(storage_)); }
bool Mutex::try_lock() { return pthread_mutex_trylock(pm(storage_)) == 0; }

RecursiveMutex::RecursiveMutex() {
  auto* m = new pthread_mutex_t;
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(m, &a);
  pthread_mutexattr_destroy(&a);
  impl_ = m;
}
RecursiveMutex::~RecursiveMutex() { auto* m = static_cast<pthread_mutex_t*>(impl_); pthread_mutex_destroy(m); delete m; }
void RecursiveMutex::lock() { pthread_mutex_lock(static_cast<pthread_mutex_t*>(impl_)); }
void RecursiveMutex::unlock() { pthread_mutex_unlock(static_cast<pthread_mutex_t*>(impl_)); }
bool RecursiveMutex::try_lock() { return pthread_mutex_trylock(static_cast<pthread_mutex_t*>(impl_)) == 0; }

static pthread_rwlock_t* rw(void* p) { return static_cast<pthread_rwlock_t*>(p); }
SharedMutex::SharedMutex() { auto* l = new pthread_rwlock_t; pthread_rwlock_init(l, nullptr); impl_ = l; }
SharedMutex::~SharedMutex() { pthread_rwlock_destroy(rw(impl_)); delete rw(impl_); }
void SharedMutex::lock() { pthread_rwlock_wrlock(rw(impl_)); }
void SharedMutex::unlock() { pthread_rwlock_unlock(rw(impl_)); }
bool SharedMutex::try_lock() { return pthread_rwlock_trywrlock(rw(impl_)) == 0; }
void SharedMutex::lock_shared() { pthread_rwlock_rdlock(rw(impl_)); }
void SharedMutex::unlock_shared() { pthread_rwlock_unlock(rw(impl_)); }
bool SharedMutex::try_lock_shared() { return pthread_rwlock_tryrdlock(rw(impl_)) == 0; }

// Timed waits run on the monotonic clock where the host has one for condition
// variables (Linux); macOS waits a relative interval, which is also immune to
// the wall clock moving.
static pthread_cond_t* pc(void* p) { return static_cast<pthread_cond_t*>(p); }
CondVar::CondVar() {
  auto* c = new pthread_cond_t;
  pthread_condattr_t a;
  pthread_condattr_init(&a);
#if !defined(__APPLE__)
  pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
#endif
  pthread_cond_init(c, &a);
  pthread_condattr_destroy(&a);
  impl_ = c;
}
CondVar::~CondVar() { pthread_cond_destroy(pc(impl_)); delete pc(impl_); }
void CondVar::notify_one() { pthread_cond_signal(pc(impl_)); }
void CondVar::notify_all() { pthread_cond_broadcast(pc(impl_)); }
void CondVar::wait_raw(Mutex& m) { pthread_cond_wait(pc(impl_), pm(reinterpret_cast<unsigned char*>(&m))); }
bool CondVar::wait_raw_ms(Mutex& m, uint32_t ms) {
  timespec ts{ms / 1000, long(ms % 1000) * 1000000L};
#if defined(__APPLE__)
  return pthread_cond_timedwait_relative_np(pc(impl_), pm(reinterpret_cast<unsigned char*>(&m)), &ts) == 0;
#else
  timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  ts.tv_sec += now.tv_sec;
  ts.tv_nsec += now.tv_nsec;
  if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
  return pthread_cond_timedwait(pc(impl_), pm(reinterpret_cast<unsigned char*>(&m)), &ts) == 0;
#endif
}

// The handle and the running thread share State; whichever lets go last frees
// it, so a detached thread cleans up after itself.
struct Thread::State {
  pthread_t handle;
  std::function<void()> body;
  std::atomic<int> refs{2};
};
static void release(Thread::State* s) { if (s->refs.fetch_sub(1) == 1) delete s; }
static void* thread_entry(void* p) { auto* s = static_cast<Thread::State*>(p); s->body(); release(s); return nullptr; }

// Every thread gets the same, explicit stack. Left to the platform it was
// 512 KB on macOS against 8 MB on Linux: a client session reading a view
// built from views runs the engine's query path inside itself two or three
// levels deep (each level a few large frames), which fit in Linux's stack and
// overflowed macOS's - a SIGSEGV in whatever function touched the guard page
// (fit_decimal_scales, building the outer view). Only what a thread touches is
// ever backed by memory.
static constexpr size_t kThreadStackBytes = (size_t)16 << 20;

Thread::Thread(std::function<void()> body) : state_(new State) {
  state_->body = std::move(body);
  pthread_attr_t attr;
  const bool have_attr = pthread_attr_init(&attr) == 0;
  if (have_attr) pthread_attr_setstacksize(&attr, kThreadStackBytes);   // on failure: the platform's default
  if (pthread_create(&state_->handle, have_attr ? &attr : nullptr, thread_entry, state_) == 0) started_ = true;
  else { delete state_; state_ = nullptr; }
  if (have_attr) pthread_attr_destroy(&attr);
}
Thread::~Thread() { if (started_) join(); }
Thread::Thread(Thread&& o) noexcept : state_(o.state_), started_(o.started_) { o.state_ = nullptr; o.started_ = false; }
Thread& Thread::operator=(Thread&& o) noexcept {
  if (this != &o) { if (started_) join(); state_ = o.state_; started_ = o.started_; o.state_ = nullptr; o.started_ = false; }
  return *this;
}
void Thread::join() { if (started_) { pthread_join(state_->handle, nullptr); started_ = false; release(state_); state_ = nullptr; } }
void Thread::detach() { if (started_) { pthread_detach(state_->handle); started_ = false; release(state_); state_ = nullptr; } }

void sleep_ms(uint32_t ms) { timespec ts{ms / 1000, long(ms % 1000) * 1000000L}; nanosleep(&ts, nullptr); }
void yield() { sched_yield(); }
void release_idle_stack() {
#if defined(__linux__)
  pthread_attr_t a;
  if (pthread_getattr_np(pthread_self(), &a) != 0) return;
  void* lo = nullptr;
  size_t size = 0;
  const bool got = pthread_attr_getstack(&a, &lo, &size) == 0;
  pthread_attr_destroy(&a);
  if (!got || (pid_t)::syscall(SYS_gettid) == ::getpid()) return;   // the main thread's stack grows on demand: nothing to give
  volatile char here = 0;
  const uintptr_t page = 4096;
  const uintptr_t keep_to = ((uintptr_t)&here - page) & ~(page - 1);   // the current frame and a page below it stay
  const uintptr_t low = ((uintptr_t)lo + page - 1) & ~(page - 1);
  if (keep_to > low) ::madvise((void*)low, keep_to - low, MADV_DONTNEED);
#endif
}
unsigned hardware_threads() { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (unsigned)n : 1u; }

}  // namespace aios::hal
