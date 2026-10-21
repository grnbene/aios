#pragma once
// WebAssembly (wasi) builds run on one thread, and wasi's libc++ is configured without threads: the
// std lock types are not declared at all. Code that guards shared state with them (ASQL, Cells) is
// correct on one thread with locks that do nothing, so this header declares exactly those, as
// no-ops. Wasm builds force-include it (-include aios/core/wasi_threads.hpp); everywhere else it is
// empty.
#if defined(__wasi__)
#include <cstddef>  // libc++'s configuration: whether it was built with threads
#endif
#if defined(__wasi__) && (defined(_LIBCPP_HAS_NO_THREADS) || (defined(_LIBCPP_HAS_THREADS) && !_LIBCPP_HAS_THREADS))
// Says so to code that leaves out the std lock headers then (they would clash with these, or not exist).
#define AIOS_WASI_NO_THREADS 1
#include <chrono>
#include <utility>

namespace std {

struct defer_lock_t { explicit defer_lock_t() = default; };
struct try_to_lock_t { explicit try_to_lock_t() = default; };
struct adopt_lock_t { explicit adopt_lock_t() = default; };
inline constexpr defer_lock_t defer_lock{};
inline constexpr try_to_lock_t try_to_lock{};
inline constexpr adopt_lock_t adopt_lock{};

struct mutex {
  constexpr mutex() noexcept = default;
  mutex(const mutex&) = delete;
  mutex& operator=(const mutex&) = delete;
  void lock() {}
  bool try_lock() { return true; }
  void unlock() {}
};
struct recursive_mutex : mutex {};
struct timed_mutex : mutex {
  template <class R, class P> bool try_lock_for(const chrono::duration<R, P>&) { return true; }
  template <class C, class D> bool try_lock_until(const chrono::time_point<C, D>&) { return true; }
};
struct recursive_timed_mutex : timed_mutex {};
struct shared_mutex : mutex {
  void lock_shared() {}
  bool try_lock_shared() { return true; }
  void unlock_shared() {}
};
struct shared_timed_mutex : shared_mutex {};

template <class M> class lock_guard {
 public:
  using mutex_type = M;
  explicit lock_guard(M& m) : m_(m) { m_.lock(); }
  lock_guard(M& m, adopt_lock_t) : m_(m) {}
  ~lock_guard() { m_.unlock(); }
  lock_guard(const lock_guard&) = delete;
  lock_guard& operator=(const lock_guard&) = delete;
 private:
  M& m_;
};

template <class M> class unique_lock {
 public:
  using mutex_type = M;
  unique_lock() noexcept = default;
  explicit unique_lock(M& m) : m_(&m), owns_(true) { m.lock(); }
  unique_lock(M& m, defer_lock_t) noexcept : m_(&m) {}
  unique_lock(M& m, try_to_lock_t) : m_(&m), owns_(m.try_lock()) {}
  unique_lock(M& m, adopt_lock_t) : m_(&m), owns_(true) {}
  unique_lock(unique_lock&& o) noexcept : m_(std::exchange(o.m_, nullptr)), owns_(std::exchange(o.owns_, false)) {}
  unique_lock& operator=(unique_lock&& o) noexcept { if (owns_) m_->unlock(); m_ = std::exchange(o.m_, nullptr); owns_ = std::exchange(o.owns_, false); return *this; }
  ~unique_lock() { if (owns_) m_->unlock(); }
  void lock() { m_->lock(); owns_ = true; }
  bool try_lock() { return owns_ = m_->try_lock(); }
  void unlock() { m_->unlock(); owns_ = false; }
  M* release() noexcept { owns_ = false; return std::exchange(m_, nullptr); }
  bool owns_lock() const noexcept { return owns_; }
  explicit operator bool() const noexcept { return owns_; }
  M* mutex() const noexcept { return m_; }
 private:
  M* m_ = nullptr;
  bool owns_ = false;
};

template <class M> class shared_lock {
 public:
  using mutex_type = M;
  shared_lock() noexcept = default;
  explicit shared_lock(M& m) : m_(&m), owns_(true) { m.lock_shared(); }
  shared_lock(M& m, defer_lock_t) noexcept : m_(&m) {}
  ~shared_lock() { if (owns_) m_->unlock_shared(); }
  void lock() { m_->lock_shared(); owns_ = true; }
  void unlock() { m_->unlock_shared(); owns_ = false; }
  bool owns_lock() const noexcept { return owns_; }
 private:
  M* m_ = nullptr;
  bool owns_ = false;
};

template <class... M> class scoped_lock {
 public:
  explicit scoped_lock(M&...) {}
  scoped_lock(const scoped_lock&) = delete;
  scoped_lock& operator=(const scoped_lock&) = delete;
};

// Nothing can be waiting on one thread: a wait returns at once, a notify does nothing.
enum class cv_status { no_timeout, timeout };
struct condition_variable {
  void notify_one() noexcept {}
  void notify_all() noexcept {}
  template <class L> void wait(L&) {}
  template <class L, class P> void wait(L&, P pred) { (void)pred(); }
  template <class L, class R, class Per> cv_status wait_for(L&, const chrono::duration<R, Per>&) { return cv_status::timeout; }
  template <class L, class R, class Per, class P> bool wait_for(L&, const chrono::duration<R, Per>&, P pred) { return pred(); }
  template <class L, class C, class D> cv_status wait_until(L&, const chrono::time_point<C, D>&) { return cv_status::timeout; }
  template <class L, class C, class D, class P> bool wait_until(L&, const chrono::time_point<C, D>&, P pred) { return pred(); }
};
using condition_variable_any = condition_variable;

}  // namespace std
#endif
