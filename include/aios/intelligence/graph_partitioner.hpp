#pragma once
#include <vector>
#include <string>
#include <memory>
#include "aios/core/result.hpp"
#include "aios/compute/compute_graph.hpp"
#include "aios/compute/device.hpp"

namespace aios::intelligence {

// Where one operation was placed and what that placement is expected to cost.
// A wave is not one device's work: operations that do not depend on each other
// run on different devices at the same time, which is the whole point of
// partitioning, so the placement is per operation and the wave only summarises.
struct OpPlacement {
  OperationID op;
  DeviceID device;
  double compute_ms = 0.0;
  double transfer_ms = 0.0;   // moving inputs produced on another device
};

struct PartitionedWave {
  uint32_t wave_index = 0;
  DeviceID target_device;     // the device carrying the most of this wave
  std::vector<OperationID> operations;
  std::vector<OpPlacement> placements;
  double estimated_latency_ms = 0.0;   // the slowest device's lane in this wave
};

// The cost model the partitioner reasons with. Compute rates come from the
// devices themselves (DeviceCapacity::flops_per_second); the interconnect is
// the only figure the caller supplies, because no device reports it.
struct PartitionCostModel {
  double link_bytes_per_second = 8e9;   // PCIe 3.0 x16 practical, one direction
  double host_bytes_per_second = 2e10;  // same device, no fabric hop
};

class GraphAutoPartitioner {
 public:
  GraphAutoPartitioner();
  explicit GraphAutoPartitioner(const PartitionCostModel& model);

  Result<std::vector<PartitionedWave>> partition_graph(
      const ComputeGraph& graph,
      const std::vector<std::shared_ptr<Device>>& available_devices);

  // The fabric hands out non-owning pointers; a partitioner has no business
  // taking ownership of a device just to be asked where an operation goes.
  Result<std::vector<PartitionedWave>> partition_graph(
      const ComputeGraph& graph,
      const std::vector<Device*>& available_devices);

  // Milliseconds the graph would take on a reference machine that sustains
  // 1 GFLOP/s and 1 GB/s. It is a property of the graph alone, so it is the
  // figure to compare two graphs with — not a prediction for any real device.
  double estimate_cost(const ComputeGraph& graph) const;

  // What one operation actually asks of a machine, derived from its opcode and
  // the shapes it names when the caller declared no estimate of its own.
  static uint64_t operation_flops(const Operation& op);
  static uint64_t operation_bytes(const Operation& op);

 private:
  PartitionCostModel model_;
};

}  // namespace aios::intelligence
