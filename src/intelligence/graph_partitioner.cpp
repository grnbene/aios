#include "aios/intelligence/graph_partitioner.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace aios::intelligence {
namespace {

// The largest input, which is the extent most element-wise opcodes work over.
uint64_t widest_input(const Operation& op) {
  uint64_t n = 0;
  for (const auto& t : op.inputs) n = std::max(n, t.elements());
  if (n == 0) for (const auto& t : op.outputs) n = std::max(n, t.elements());
  return n;
}

// A matrix multiply's shape is the one case where the operands say something
// the element count cannot: out[MxN] = in0[MxK] · in1[KxN] is 2·M·N·K, and
// reading M·N off the output alone understates it by a factor of K.
uint64_t matmul_flops(const Operation& op) {
  if (op.inputs.size() < 2 || op.inputs[0].rank() < 2 || op.inputs[1].rank() < 2) {
    return 2 * widest_input(op);
  }
  const auto& a = op.inputs[0].shape;
  const auto& b = op.inputs[1].shape;
  uint64_t m = a[a.size() - 2], k = a[a.size() - 1], n = b[b.size() - 1];
  uint64_t batch = 1;
  for (size_t i = 0; i + 2 < a.size(); ++i) batch *= a[i];
  return 2ull * batch * m * k * n;
}

// The same dependency rules ComputeGraph::order() enforces: declared edges,
// plus write→read, read→write and write→write hazards on shared memory.
std::map<OperationID, std::set<OperationID>> dependencies(const ComputeGraph& g) {
  std::map<OperationID, std::set<OperationID>> deps;
  std::map<MemoryID, std::vector<size_t>> writers, readers;
  for (size_t i = 0; i < g.operations.size(); ++i) {
    const auto& op = g.operations[i];
    auto& d = deps[op.id];
    for (const auto& e : op.depends) d.insert(e);
    for (const auto& t : op.inputs) for (size_t w : writers[t.memory]) d.insert(g.operations[w].id);
    for (const auto& t : op.outputs) {
      for (size_t w : writers[t.memory]) d.insert(g.operations[w].id);
      for (size_t rd : readers[t.memory]) if (rd != i) d.insert(g.operations[rd].id);
    }
    for (const auto& t : op.inputs) readers[t.memory].push_back(i);
    for (const auto& t : op.outputs) writers[t.memory].push_back(i);
    d.erase(op.id);
  }
  return deps;
}

bool selector_matches(const std::string& selector, const Device& d) {
  if (selector.empty()) return true;
  if (selector == d.name()) return true;
  if (selector == device_type_name(d.type())) return true;
  return d.name().rfind(selector, 0) == 0 || d.id().short_str().rfind(selector, 0) == 0;
}

bool device_admits(const Device& d, const Operation& op) {
  const auto& req = op.requirement;
  if (!req.affinity.is_null() && !(d.id() == req.affinity)) return false;
  if (req.device != DeviceType::Any && d.type() != req.device) return false;
  if (!selector_matches(req.device_selector, d)) return false;
  if (req.memory > 0 && d.capacity().memory > 0 && req.memory > d.capacity().memory) return false;
  return d.supports(op);
}

}  // namespace

uint64_t GraphAutoPartitioner::operation_flops(const Operation& op) {
  if (op.requirement.flops > 0) return op.requirement.flops;
  uint64_t n = widest_input(op);
  switch (op.code) {
    case OpCode::Nop:       return 0;
    case OpCode::Copy:      return 0;
    case OpCode::Fill:      return n;
    case OpCode::Add:
    case OpCode::Mul:
    case OpCode::Scale:
    case OpCode::Relu:
    case OpCode::ReluGrad:
    case OpCode::BiasAdd:
    case OpCode::BiasAddGrad: return n;
    case OpCode::Gelu:      return 8 * n;
    case OpCode::GeluGrad:  return 12 * n;
    case OpCode::MatMul:
    case OpCode::MatMulGradA:
    case OpCode::MatMulGradB: return matmul_flops(op);
    case OpCode::ReduceSum: return n;
    case OpCode::LayerNorm: return 6 * n;
    case OpCode::LayerNormGrad: return 10 * n;
    case OpCode::SoftmaxCrossEntropyLoss: return 4 * n;
    case OpCode::SoftmaxCrossEntropyGrad: return 2 * n;
    case OpCode::AdamStep:  return 6 * n;
    // QK^T and att·V over the causal half: T*T*D each, with n = T*D of q.
    case OpCode::CausalAttention:
    case OpCode::CausalAttentionGrad: {
      uint64_t T = op.inputs.empty() || op.inputs[0].rank() < 2 ? 1 : op.inputs[0].shape[0];
      return (op.code == OpCode::CausalAttention ? 1 : 2) * n * T;
    }
    // An exponential and a divide per element, then the reduction between them.
    case OpCode::Softmax:   return 3 * n;
    case OpCode::Histogram: return n;
    case OpCode::Normalize: return 2 * n;
    case OpCode::Custom:    return n;
  }
  return n;
}

uint64_t GraphAutoPartitioner::operation_bytes(const Operation& op) {
  uint64_t b = 0;
  for (const auto& t : op.inputs) b += t.bytes();
  for (const auto& t : op.outputs) b += t.bytes();
  return b;
}

GraphAutoPartitioner::GraphAutoPartitioner() = default;
GraphAutoPartitioner::GraphAutoPartitioner(const PartitionCostModel& model) : model_(model) {}

Result<std::vector<PartitionedWave>> GraphAutoPartitioner::partition_graph(
    const ComputeGraph& graph,
    const std::vector<std::shared_ptr<Device>>& available_devices) {
  std::vector<Device*> raw;
  for (const auto& d : available_devices) if (d) raw.push_back(d.get());
  return partition_graph(graph, raw);
}

Result<std::vector<PartitionedWave>> GraphAutoPartitioner::partition_graph(
    const ComputeGraph& graph,
    const std::vector<Device*>& available_devices) {
  std::vector<PartitionedWave> waves;
  if (graph.operations.empty()) return waves;

  std::vector<Device*> devices;
  for (Device* d : available_devices) if (d) devices.push_back(d);
  if (devices.empty()) {
    return Result<std::vector<PartitionedWave>>(Status::NotFound, "no devices to partition onto");
  }

  auto deps = dependencies(graph);
  std::set<OperationID> known;
  for (const auto& op : graph.operations) known.insert(op.id);
  for (const auto& [id, set] : deps) {
    for (const auto& d : set) {
      if (!known.count(d)) {
        return Result<std::vector<PartitionedWave>>(Status::Invalid,
            "operation depends on unknown operation " + d.short_str());
      }
    }
  }

  // A wave is a dependency level: the longest chain of predecessors an
  // operation has. Everything at one level is genuinely independent and may
  // run at the same time, which is what makes the level a unit of work.
  std::map<OperationID, uint32_t> level;
  size_t settled = 0;
  bool progress = true;
  while (settled < graph.operations.size() && progress) {
    progress = false;
    for (const auto& op : graph.operations) {
      if (level.count(op.id)) continue;
      uint32_t lv = 0;
      bool ready = true;
      for (const auto& d : deps[op.id]) {
        auto it = level.find(d);
        if (it == level.end()) { ready = false; break; }
        lv = std::max(lv, it->second + 1);
      }
      if (ready) { level[op.id] = lv; settled++; progress = true; }
    }
  }
  if (settled < graph.operations.size()) {
    return Result<std::vector<PartitionedWave>>(Status::Invalid, "graph has a dependency cycle");
  }

  uint32_t last_level = 0;
  for (const auto& [id, lv] : level) last_level = std::max(last_level, lv);

  // Where each memory object was last written, so a consumer placed elsewhere
  // pays for the hop instead of pretending the bytes were already there.
  std::map<MemoryID, DeviceID> resident;

  for (uint32_t lv = 0; lv <= last_level; ++lv) {
    PartitionedWave wave;
    wave.wave_index = lv;

    std::vector<const Operation*> members;
    for (const auto& op : graph.operations) if (level[op.id] == lv) members.push_back(&op);
    if (members.empty()) continue;

    // Heaviest first: a list scheduler that places the small work first leaves
    // the one long operation to land on an already-loaded device.
    std::sort(members.begin(), members.end(), [](const Operation* a, const Operation* b) {
      uint64_t fa = operation_flops(*a), fb = operation_flops(*b);
      if (fa != fb) return fa > fb;
      return a->requirement.priority > b->requirement.priority;
    });

    std::map<DeviceID, double> lane_ms;      // when each device frees up in this wave
    std::map<DeviceID, size_t> op_count;
    std::map<MemoryID, DeviceID> writes_here;

    for (const Operation* op : members) {
      uint64_t flops = operation_flops(*op);
      Device* chosen = nullptr;
      double chosen_compute = 0.0, chosen_transfer = 0.0, chosen_finish = 0.0;

      for (Device* d : devices) {
        if (!device_admits(*d, *op)) continue;
        uint64_t rate = d->capacity().flops_per_second;
        if (rate == 0) continue;
        double compute_ms = (static_cast<double>(flops) / static_cast<double>(rate)) * 1000.0;

        double transfer_ms = 0.0;
        for (const auto& t : op->inputs) {
          auto it = resident.find(t.memory);
          if (it == resident.end() || it->second == d->id()) continue;
          if (model_.link_bytes_per_second > 0) {
            transfer_ms += (static_cast<double>(t.bytes()) / model_.link_bytes_per_second) * 1000.0;
          }
        }

        double finish = lane_ms[d->id()] + transfer_ms + compute_ms;
        if (!chosen || finish < chosen_finish) {
          chosen = d;
          chosen_compute = compute_ms;
          chosen_transfer = transfer_ms;
          chosen_finish = finish;
        }
      }

      if (!chosen) {
        return Result<std::vector<PartitionedWave>>(Status::Unsupported,
            "no available device can run operation " + op->id.short_str() +
            " (" + opcode_name(op->code) + ")");
      }

      lane_ms[chosen->id()] = chosen_finish;
      op_count[chosen->id()]++;
      for (const auto& t : op->outputs) writes_here[t.memory] = chosen->id();

      OpPlacement p;
      p.op = op->id;
      p.device = chosen->id();
      p.compute_ms = chosen_compute;
      p.transfer_ms = chosen_transfer;
      wave.placements.push_back(p);
      wave.operations.push_back(op->id);
    }

    for (const auto& [mem, dev] : writes_here) resident[mem] = dev;

    double slowest = 0.0;
    size_t most = 0;
    for (const auto& [dev, ms] : lane_ms) {
      slowest = std::max(slowest, ms);
      if (op_count[dev] > most) { most = op_count[dev]; wave.target_device = dev; }
    }
    wave.estimated_latency_ms = slowest;
    waves.push_back(std::move(wave));
  }

  return waves;
}

double GraphAutoPartitioner::estimate_cost(const ComputeGraph& graph) const {
  // The reference machine: one billion floating point operations a second and
  // one gigabyte a second of memory traffic. Whichever of the two the graph is
  // bound by decides its time, so the cost is the larger.
  double flop_ms = 0.0, byte_ms = 0.0;
  for (const auto& op : graph.operations) {
    flop_ms += static_cast<double>(operation_flops(op)) / 1e6;
    byte_ms += static_cast<double>(operation_bytes(op)) / 1e6;
  }
  return std::max(flop_ms, byte_ms);
}

}  // namespace aios::intelligence
