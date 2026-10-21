#include "aios/kernel/time_quota.hpp"

#include <algorithm>

namespace aios {

PreemptiveSchedulerQuota::PreemptiveSchedulerQuota() {}

// A window that has run out is opened again, however many windows went by
// while the entity was idle. Advancing by whole periods rather than to `now`
// keeps the boundaries where they were set, so an entity cannot buy itself a
// fresh budget by pausing just before one.
void PreemptiveSchedulerQuota::roll(ExecutionQuota& q, uint64_t now_ns) const {
  if (q.period_ns == 0) return;
  if (q.period_started_ns == 0) { q.period_started_ns = now_ns; q.periods = 1; return; }
  if (now_ns < q.period_started_ns + q.period_ns) return;
  uint64_t windows = (now_ns - q.period_started_ns) / q.period_ns;
  q.period_started_ns += windows * q.period_ns;
  q.periods += windows;
  q.used_ns = 0;
  q.preempted = false;
}

Result<void> PreemptiveSchedulerQuota::assign_quota(const EntityID& entity, uint64_t timeslice_ns,
                                                    uint64_t budget_ns, uint64_t period_ns) {
  if (entity.is_null()) return Result<void>(Status::Invalid, "a quota needs an entity");
  if (period_ns == 0) return Result<void>(Status::Invalid, "a quota period of zero never refills");
  if (budget_ns == 0) return Result<void>(Status::Invalid, "a budget of zero would never let the entity run");
  hal::LockGuard l(mu_);
  ExecutionQuota q;
  q.timeslice_ns = timeslice_ns ? timeslice_ns : budget_ns;
  q.budget_ns = budget_ns;
  q.period_ns = period_ns;
  quotas_[entity] = q;
  return Ok();
}

Result<void> PreemptiveSchedulerQuota::clear_quota(const EntityID& entity) {
  hal::LockGuard l(mu_);
  if (!quotas_.erase(entity)) return Result<void>(Status::NotFound, "no quota on " + entity.short_str());
  return Ok();
}

bool PreemptiveSchedulerQuota::may_run(const EntityID& entity, uint64_t now_ns) {
  hal::LockGuard l(mu_);
  auto it = quotas_.find(entity);
  if (it == quotas_.end()) return true;
  roll(it->second, now_ns);
  return it->second.used_ns < it->second.budget_ns;
}

bool PreemptiveSchedulerQuota::charge(const EntityID& entity, uint64_t now_ns, uint64_t ns, uint64_t ops) {
  hal::LockGuard l(mu_);
  auto it = quotas_.find(entity);
  if (it == quotas_.end()) return true;
  auto& q = it->second;
  roll(q, now_ns);
  q.used_ns += ns;
  q.charged_ops += ops;
  if (q.used_ns >= q.budget_ns) {
    if (!q.preempted) q.preemptions++;
    q.preempted = true;
    return false;
  }
  return true;
}

uint64_t PreemptiveSchedulerQuota::runnable_ns(const EntityID& entity, uint64_t now_ns) {
  hal::LockGuard l(mu_);
  auto it = quotas_.find(entity);
  if (it == quotas_.end()) return UINT64_MAX;
  auto& q = it->second;
  roll(q, now_ns);
  if (q.used_ns >= q.budget_ns) return 0;
  return std::min(q.timeslice_ns, q.budget_ns - q.used_ns);
}

void PreemptiveSchedulerQuota::reset_period() {
  hal::LockGuard l(mu_);
  for (auto& [id, q] : quotas_) { q.used_ns = 0; q.preempted = false; q.period_started_ns = 0; }
}

bool PreemptiveSchedulerQuota::has_quota(const EntityID& entity) const {
  hal::LockGuard l(mu_);
  return quotas_.count(entity) != 0;
}

Result<ExecutionQuota> PreemptiveSchedulerQuota::get_quota(const EntityID& entity) const {
  hal::LockGuard l(mu_);
  auto it = quotas_.find(entity);
  if (it == quotas_.end()) return Result<ExecutionQuota>(Status::NotFound, "no quota on " + entity.short_str());
  return it->second;
}

std::vector<QuotaEntry> PreemptiveSchedulerQuota::list() const {
  hal::LockGuard l(mu_);
  std::vector<QuotaEntry> out;
  for (const auto& [id, q] : quotas_) out.push_back(QuotaEntry{id, q});
  return out;
}

}  // namespace aios
