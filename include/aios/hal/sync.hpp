#pragma once
// Threads and locks are host facilities. The kernel core uses these types and
// never names a host threading API; hal/ provides them (pthread on Linux, a
// scheduler-native implementation on bare metal).
#include <atomic>
#include <cstdint>
#include <functional>

namespace aios::hal {

class Mutex {
 public:
  Mutex();
  ~Mutex();
  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;
  void lock();
  void unlock();
  bool try_lock();
 private:
  alignas(8) unsigned char storage_[64];
};

// The same lock taken again by the thread that holds it does not deadlock.
class RecursiveMutex {
 public:
  RecursiveMutex();
  ~RecursiveMutex();
  RecursiveMutex(const RecursiveMutex&) = delete;
  RecursiveMutex& operator=(const RecursiveMutex&) = delete;
  void lock();
  void unlock();
  bool try_lock();
 private:
  void* impl_;
};

// Many readers or one writer. Works with std::shared_lock / std::unique_lock.
class SharedMutex {
 public:
  SharedMutex();
  ~SharedMutex();
  SharedMutex(const SharedMutex&) = delete;
  SharedMutex& operator=(const SharedMutex&) = delete;
  void lock();
  void unlock();
  bool try_lock();
  void lock_shared();
  void unlock_shared();
  bool try_lock_shared();
 private:
  void* impl_;
};

// Waits on a hal::Mutex held through std::unique_lock<hal::Mutex>.
class CondVar {
 public:
  CondVar();
  ~CondVar();
  CondVar(const CondVar&) = delete;
  CondVar& operator=(const CondVar&) = delete;
  template <class Lock> void wait(Lock& lk) { wait_raw(*lk.mutex()); }
  template <class Lock, class Pred> void wait(Lock& lk, Pred pred) { while (!pred()) wait_raw(*lk.mutex()); }
  // false when the time ran out before a notify.
  template <class Lock> bool wait_for_ms(Lock& lk, uint32_t ms) { return wait_raw_ms(*lk.mutex(), ms); }
  template <class Lock, class Pred> bool wait_for_ms(Lock& lk, uint32_t ms, Pred pred) {
    while (!pred()) if (!wait_raw_ms(*lk.mutex(), ms)) return pred();
    return true;
  }
  void notify_one();
  void notify_all();
 private:
  void wait_raw(Mutex& m);
  bool wait_raw_ms(Mutex& m, uint32_t ms);
  void* impl_;
};

class LockGuard {
 public:
  explicit LockGuard(Mutex& m) : m_(m) { m_.lock(); }
  ~LockGuard() { m_.unlock(); }
  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;
 private:
  Mutex& m_;
};

class Thread {
 public:
  Thread() = default;
  explicit Thread(std::function<void()> body);
  ~Thread();
  Thread(Thread&& o) noexcept;
  Thread& operator=(Thread&& o) noexcept;
  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;
  bool joinable() const { return started_; }
  void join();
  // Let the body run on without this handle; it must not touch the caller's stack.
  void detach();
  struct State;
 private:
  State* state_ = nullptr;
  bool started_ = false;
};

void sleep_ms(uint32_t ms);
void yield();
// Gives back the calling thread's stack below its current frame (a few KB
// kept), for a thread about to sit idle: what a deep call once touched stays
// resident otherwise, ~60 KB for each idle SQL session. No-op where the
// platform cannot.
void release_idle_stack();

// Commit flushes in progress (the redo log's fdatasync), process-wide. A bulk
// background writer (a checkpoint's table files) waits for them between its
// chunks: otherwise a commit's flush queues behind megabytes of table data
// on the same disk (3-70 ms commits through every checkpoint).
inline std::atomic<int>& foreground_io() { static std::atomic<int> n{0}; return n; }
struct ForegroundIo {
  ForegroundIo() { foreground_io().fetch_add(1, std::memory_order_acq_rel); }
  ~ForegroundIo() { foreground_io().fetch_sub(1, std::memory_order_acq_rel); }
  ForegroundIo(const ForegroundIo&) = delete;
  ForegroundIo& operator=(const ForegroundIo&) = delete;
};
// Hardware threads that can run at once (cores online on bare metal).
unsigned hardware_threads();

}  // namespace aios::hal
