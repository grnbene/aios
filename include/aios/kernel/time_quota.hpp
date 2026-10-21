#pragma once
// Preemptive execution quotas. An entity is charged for the execution time its
// operations actually consume, measured by the scheduler from the device
// samples it already takes. When an entity has spent its budget for the current
// period the scheduler stops its graph where it stands and returns a resumable
// position — which is what preemption means in a machine whose unit of work is
// a resumable graph rather than a thread.
#include <cstdint>
#include <vector>
#include <unordered_map>
#include "aios/core/ids.hpp"
#include "aios/core/result.hpp"
#include "aios/hal/sync.hpp"

namespace aios {

struct ExecutionQuota {
  uint64_t timeslice_ns = 10000000;      // longest single run before yielding
  uint64_t period_ns = 1000000000;       // the accounting window
  uint64_t budget_ns = 1000000000;       // execution time allowed inside one window
  uint64_t used_ns = 0;                  // charged so far in this window
  uint64_t period_started_ns = 0;
  uint64_t periods = 0;                  // windows that have opened
  uint64_t preemptions = 0;              // times the scheduler stopped this entity
  uint64_t charged_ops = 0;
  bool preempted = false;                // out of budget as of the last charge
};

struct QuotaEntry {
  EntityID entity;
  ExecutionQuota quota;
};

class PreemptiveSchedulerQuota {
 public:
  PreemptiveSchedulerQuota();

  Result<void> assign_quota(const EntityID& entity, uint64_t timeslice_ns, uint64_t budget_ns,
                            uint64_t period_ns = 1000000000);
  Result<void> clear_quota(const EntityID& entity);

  // Whether the entity may start (or carry on) at this instant. An entity with
  // no quota is unconstrained, which is how the machine behaves for everything
  // nobody has bounded.
  bool may_run(const EntityID& entity, uint64_t now_ns);

  // Charge measured execution time. Returns false when the entity has just
  // exhausted its window and must be stopped.
  bool charge(const EntityID& entity, uint64_t now_ns, uint64_t ns, uint64_t ops = 1);

  // How long the entity may run in one uninterrupted stretch from now: the
  // smaller of its timeslice and what is left of its budget. Zero means stop.
  uint64_t runnable_ns(const EntityID& entity, uint64_t now_ns);

  void reset_period();
  bool has_quota(const EntityID& entity) const;
  Result<ExecutionQuota> get_quota(const EntityID& entity) const;
  std::vector<QuotaEntry> list() const;

 private:
  void roll(ExecutionQuota& q, uint64_t now_ns) const;
  mutable hal::Mutex mu_;
  std::unordered_map<EntityID, ExecutionQuota, Id128Hash> quotas_;
};

}  // namespace aios
