#pragma once
// The Scheduler decides WHERE, WHEN and HOW each operation runs, weighing
// device availability, memory locality, priority, dependencies and the
// owning entity's capabilities. It executes graphs in dependency waves and
// records the owner's execution position so a graph can be checkpointed
// mid-flight and resumed elsewhere.
#include "aios/compute/compute_graph.hpp"
#include "aios/compute/device.hpp"
#include "aios/capability/capability.hpp"
#include "aios/hal/host.hpp"
#include "aios/kernel/time_quota.hpp"
#include "aios/hal/sync.hpp"
#include <map>
#include <tuple>

namespace aios {

struct Placement {
  OperationID op;
  DeviceID device;
  std::string device_name;
  uint32_t lanes = 1;
  uint32_t wave = 0;
  std::string reason;     // why this device
  uint64_t migrated_bytes = 0;
};

struct ExecutionReport {
  GraphID graph;
  uint64_t operations_total = 0;
  uint64_t operations_run = 0;
  uint64_t position = 0;          // completed operations after this run
  bool complete = false;
  uint64_t elapsed_ns = 0;
  // The entity ran out of its execution budget (or its timeslice) and the
  // scheduler stopped the graph where it stands. Not a failure: `position` is
  // where a later run picks it up.
  bool preempted = false;
  uint64_t charged_ns = 0;
  std::vector<Placement> placements;
  std::vector<ExecutionSample> samples;
  std::string failure;
};

struct SchedulePolicy {
  uint64_t max_operations = 0;    // 0 = run to completion; otherwise step budget (resumable)
  bool allow_migration = true;    // move memory toward the chosen device
};

class Scheduler {
 public:
  Scheduler(hal::Host& host, DeviceFabric& devices, MemoryFabric& memory, CapabilityManager& caps,
            PreemptiveSchedulerQuota& quotas);
  PreemptiveSchedulerQuota& quotas() { return quotas_; }
  Result<std::vector<Placement>> plan(const ComputeGraph& g, const EntityID& owner) const;
  Result<ExecutionReport> execute(const ComputeGraph& g, const EntityID& owner, uint64_t start_position, const SchedulePolicy& policy);
 private:
  Result<Placement> place(const Operation& op, const EntityID& owner) const;
  // What an operation costs on a device, measured: the first time an
  // operation kind can go to more than one device, a scratch copy of that very
  // operation (same shapes, scratch memory - never the graph's own tensors)
  // runs on each candidate in turn, alone, warm-up excluded. Other sizes of
  // the kind scale with their work. A device already 20x slower than the best
  // at warm-up is not timed further. Never assumed from a device's type.
  // Kinds are measured per size class (a factor of four in work), so a big
  // matrix product can still find the GPU a small one is wasted on. A device
  // whose fixed cost per operation (measured once, on a 16-element add)
  // already exceeds the CPU's whole time for a kind is not run for it.
  struct KindCost { double ns = -1; double work = 1; };
  double predict_ns(const Operation& op, const std::vector<Device*>& candidates, size_t which, const EntityID& owner) const;
  double overhead_ns(Device* d, const EntityID& owner) const;   // (cost_mu_ held)
  double time_op(Device* d, const Operation& scratch, double give_up_after_ns) const;
  mutable std::map<std::tuple<DeviceID, uint16_t, int>, KindCost> kind_costs_;
  mutable std::map<DeviceID, double> overhead_;
  // A device's free memory, asked at most every 100 ms: the query is a driver
  // call (~1.1 ms on CUDA), and placing a training step asked it for every
  // operation on every device - 16.6 of a 19.9 s run.
  Result<DeviceMemoryStats> memory_stats_of(Device* d) const;
  mutable std::map<DeviceID, std::pair<uint64_t, DeviceMemoryStats>> mem_stats_;
  mutable hal::Mutex mem_mu_;
  mutable hal::Mutex cost_mu_;
  hal::Host& host_;
  DeviceFabric& devices_;
  MemoryFabric& memory_;
  CapabilityManager& caps_;
  PreemptiveSchedulerQuota& quotas_;
};

}  // namespace aios
