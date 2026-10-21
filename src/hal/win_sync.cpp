// Threads and locks on Windows: same contract as posix_sync.cpp (which this
// host does not compile — see CMakeLists.txt), built on CRITICAL_SECTION,
// SRWLOCK, CONDITION_VARIABLE and CreateThread instead of their pthread
// equivalents.
#include "aios/hal/sync.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <atomic>
#include <new>

namespace aios::hal {

static_assert(sizeof(CRITICAL_SECTION) <= 64, "mutex storage");
static CRITICAL_SECTION* cs(unsigned char* s) { return reinterpret_cast<CRITICAL_SECTION*>(s); }

Mutex::Mutex() { ::InitializeCriticalSection(cs(storage_)); }
Mutex::~Mutex() { ::DeleteCriticalSection(cs(storage_)); }
void Mutex::lock() { ::EnterCriticalSection(cs(storage_)); }
void Mutex::unlock() { ::LeaveCriticalSection(cs(storage_)); }
bool Mutex::try_lock() { return ::TryEnterCriticalSection(cs(storage_)) != 0; }

// A Windows CRITICAL_SECTION is already recursive (the owning thread may
// re-enter it), so RecursiveMutex is the same primitive behind a separate type.
RecursiveMutex::RecursiveMutex() { auto* c = new CRITICAL_SECTION; ::InitializeCriticalSection(c); impl_ = c; }
RecursiveMutex::~RecursiveMutex() { auto* c = static_cast<CRITICAL_SECTION*>(impl_); ::DeleteCriticalSection(c); delete c; }
void RecursiveMutex::lock() { ::EnterCriticalSection(static_cast<CRITICAL_SECTION*>(impl_)); }
void RecursiveMutex::unlock() { ::LeaveCriticalSection(static_cast<CRITICAL_SECTION*>(impl_)); }
bool RecursiveMutex::try_lock() { return ::TryEnterCriticalSection(static_cast<CRITICAL_SECTION*>(impl_)) != 0; }

static SRWLOCK* srw(void* p) { return static_cast<SRWLOCK*>(p); }
SharedMutex::SharedMutex() { auto* l = new SRWLOCK; ::InitializeSRWLock(l); impl_ = l; }
SharedMutex::~SharedMutex() { delete srw(impl_); }
void SharedMutex::lock() { ::AcquireSRWLockExclusive(srw(impl_)); }
void SharedMutex::unlock() { ::ReleaseSRWLockExclusive(srw(impl_)); }
bool SharedMutex::try_lock() { return ::TryAcquireSRWLockExclusive(srw(impl_)) != 0; }
void SharedMutex::lock_shared() { ::AcquireSRWLockShared(srw(impl_)); }
void SharedMutex::unlock_shared() { ::ReleaseSRWLockShared(srw(impl_)); }
bool SharedMutex::try_lock_shared() { return ::TryAcquireSRWLockShared(srw(impl_)) != 0; }

// wait_raw takes the hal::Mutex by reference and reaches its CRITICAL_SECTION
// the same way posix_sync.cpp reaches the pthread_mutex_t inside it: storage_
// is the type's only member, so the object's address is the primitive's address.
static CONDITION_VARIABLE* ccv(void* p) { return static_cast<CONDITION_VARIABLE*>(p); }
CondVar::CondVar() { auto* c = new CONDITION_VARIABLE; ::InitializeConditionVariable(c); impl_ = c; }
CondVar::~CondVar() { delete ccv(impl_); }
void CondVar::notify_one() { ::WakeConditionVariable(ccv(impl_)); }
void CondVar::notify_all() { ::WakeAllConditionVariable(ccv(impl_)); }
void CondVar::wait_raw(Mutex& m) { ::SleepConditionVariableCS(ccv(impl_), cs(reinterpret_cast<unsigned char*>(&m)), INFINITE); }
bool CondVar::wait_raw_ms(Mutex& m, uint32_t ms) {
  return ::SleepConditionVariableCS(ccv(impl_), cs(reinterpret_cast<unsigned char*>(&m)), ms) != 0;
}

// The handle and the running thread share State; whichever lets go last frees
// it, so a detached thread cleans up after itself (mirrors posix_sync.cpp).
struct Thread::State {
  HANDLE handle;
  std::function<void()> body;
  std::atomic<int> refs{2};
};
static void release(Thread::State* s) { if (s->refs.fetch_sub(1) == 1) delete s; }
static DWORD WINAPI thread_entry(LPVOID p) { auto* s = static_cast<Thread::State*>(p); s->body(); release(s); return 0; }

Thread::Thread(std::function<void()> body) : state_(new State) {
  state_->body = std::move(body);
  state_->handle = ::CreateThread(nullptr, 0, thread_entry, state_, 0, nullptr);
  if (state_->handle) started_ = true;
  else { delete state_; state_ = nullptr; }
}
Thread::~Thread() { if (started_) join(); }
Thread::Thread(Thread&& o) noexcept : state_(o.state_), started_(o.started_) { o.state_ = nullptr; o.started_ = false; }
Thread& Thread::operator=(Thread&& o) noexcept {
  if (this != &o) { if (started_) join(); state_ = o.state_; started_ = o.started_; o.state_ = nullptr; o.started_ = false; }
  return *this;
}
void Thread::join() { if (started_) { ::WaitForSingleObject(state_->handle, INFINITE); ::CloseHandle(state_->handle); started_ = false; release(state_); state_ = nullptr; } }
void Thread::detach() { if (started_) { ::CloseHandle(state_->handle); started_ = false; release(state_); state_ = nullptr; } }

void release_idle_stack() {}
void sleep_ms(uint32_t ms) { ::Sleep(ms); }
void yield() { ::SwitchToThread(); }
unsigned hardware_threads() { SYSTEM_INFO si; ::GetSystemInfo(&si); return si.dwNumberOfProcessors > 0 ? si.dwNumberOfProcessors : 1u; }

}  // namespace aios::hal
