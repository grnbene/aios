#include "aios/hal/lane_pool.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

#include <mutex>   // std::unique_lock / std::lock_guard: templates over hal::Mutex
#include "aios/hal/sync.hpp"

namespace aios::hal {

#if !defined(AIOS_METAL)
namespace {
thread_local bool t_inside = false;   // running a task: a nested batch runs inline

struct Job {
  const std::function<void(uint32_t)>* task = nullptr;
  uint32_t n = 0;
  std::atomic<uint32_t> next{0}, finished{0};
};

struct Pool {
  std::vector<aios::hal::Thread> workers;
  aios::hal::Mutex mu, busy;
  aios::hal::CondVar wake, done;
  std::shared_ptr<Job> job;
  uint64_t gen = 0;

  Pool() {
    const unsigned hw = std::max(1u, aios::hal::hardware_threads());
    for (unsigned i = 0; i + 1 < hw; ++i) workers.emplace_back([this] { loop(); });
  }
  void work(Job& j) {
    const bool was = t_inside;
    t_inside = true;
    for (;;) {
      const uint32_t i = j.next.fetch_add(1);
      if (i >= j.n) break;
      (*j.task)(i);
      if (j.finished.fetch_add(1) + 1 == j.n) { std::lock_guard<aios::hal::Mutex> g(mu); done.notify_all(); }
    }
    t_inside = was;
  }
  void loop() {
    uint64_t seen = 0;
    for (;;) {
      std::shared_ptr<Job> j;
      {
        std::unique_lock<aios::hal::Mutex> l(mu);
        wake.wait(l, [&] { return gen != seen; });
        seen = gen;
        j = job;
      }
      if (j) work(*j);   // a late worker finds the job's indexes spent and goes back to sleep
    }
  }
};
Pool& pool() {
  static Pool* p = new Pool();   // never torn down: its threads sleep through exit
  return *p;
}
}  // namespace

uint32_t task_lanes() { return (uint32_t)pool().workers.size() + 1; }

void run_tasks(uint32_t n, const std::function<void(uint32_t)>& task) {
  if (n == 0) return;
  Pool& p = pool();
  std::unique_lock<aios::hal::Mutex> own(p.busy, std::defer_lock);
  if (n == 1 || t_inside || p.workers.empty() || !own.try_lock()) {
    for (uint32_t i = 0; i < n; ++i) task(i);
    return;
  }
  auto j = std::make_shared<Job>();
  j->task = &task;
  j->n = n;
  {
    std::lock_guard<aios::hal::Mutex> g(p.mu);
    p.job = j;
    ++p.gen;
  }
  p.wake.notify_all();
  p.work(*j);
  std::unique_lock<aios::hal::Mutex> l(p.mu);
  p.done.wait(l, [&] { return j->finished.load() == j->n; });
  p.job.reset();
}

#else

uint32_t task_lanes() { return 4; }

void run_tasks(uint32_t n, const std::function<void(uint32_t)>& task) {
  if (n == 0) return;
  if (n == 1) { task(0); return; }
  std::vector<Thread> ts;
  for (uint32_t i = 0; i < n; ++i) ts.emplace_back([&task, i] { task(i); });
  for (auto& t : ts) t.join();
}

#endif

}  // namespace aios::hal
