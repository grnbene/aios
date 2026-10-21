#include "aios/compute/compute_graph.hpp"
#include "aios/hal/sync.hpp"
#include <algorithm>
#include <set>
#include <map>

namespace aios {

const char* device_type_name(DeviceType t) {
  switch (t) { case DeviceType::Any: return "any"; case DeviceType::CPU: return "cpu"; case DeviceType::GPU: return "gpu"; case DeviceType::NPU: return "npu";
    case DeviceType::RemoteCPU: return "remote-cpu"; case DeviceType::RemoteGPU: return "remote-gpu"; case DeviceType::RemoteNPU: return "remote-npu"; }
  return "?";
}
bool parse_device_type(const std::string& s, DeviceType& out) {
  if (s == "any") out = DeviceType::Any; else if (s == "cpu") out = DeviceType::CPU; else if (s == "gpu") out = DeviceType::GPU; else if (s == "npu") out = DeviceType::NPU;
  else if (s == "remote-cpu") out = DeviceType::RemoteCPU; else if (s == "remote-gpu") out = DeviceType::RemoteGPU; else if (s == "remote-npu") out = DeviceType::RemoteNPU; else return false;
  return true;
}
const char* opcode_name(OpCode c) {
  switch (c) {
    case OpCode::Nop: return "nop"; case OpCode::Copy: return "copy"; case OpCode::Fill: return "fill";
    case OpCode::Add: return "add"; case OpCode::Mul: return "mul"; case OpCode::Scale: return "scale";
    case OpCode::MatMul: return "matmul"; case OpCode::ReduceSum: return "reduce-sum"; case OpCode::Relu: return "relu";
    case OpCode::Softmax: return "softmax"; case OpCode::Histogram: return "histogram"; case OpCode::Normalize: return "normalize";
    case OpCode::Gelu: return "gelu"; case OpCode::GeluGrad: return "gelu-grad"; case OpCode::ReluGrad: return "relu-grad";
    case OpCode::MatMulGradA: return "matmul-grad-a"; case OpCode::MatMulGradB: return "matmul-grad-b";
    case OpCode::BiasAdd: return "bias-add"; case OpCode::BiasAddGrad: return "bias-add-grad";
    case OpCode::LayerNorm: return "layernorm"; case OpCode::LayerNormGrad: return "layernorm-grad";
    case OpCode::SoftmaxCrossEntropyLoss: return "softmax-ce-loss"; case OpCode::SoftmaxCrossEntropyGrad: return "softmax-ce-grad";
    case OpCode::AdamStep: return "adam-step";
    case OpCode::CausalAttention: return "causal-attention"; case OpCode::CausalAttentionGrad: return "causal-attention-grad";
    case OpCode::Custom: return "custom";
  }
  return "?";
}
bool parse_opcode(const std::string& s, OpCode& out) {
  static const std::pair<const char*, OpCode> t[] = {
    {"nop", OpCode::Nop}, {"copy", OpCode::Copy}, {"fill", OpCode::Fill}, {"add", OpCode::Add}, {"mul", OpCode::Mul}, {"scale", OpCode::Scale},
    {"matmul", OpCode::MatMul}, {"reduce-sum", OpCode::ReduceSum}, {"relu", OpCode::Relu}, {"softmax", OpCode::Softmax}, {"histogram", OpCode::Histogram}, {"normalize", OpCode::Normalize},
    {"gelu", OpCode::Gelu}, {"gelu-grad", OpCode::GeluGrad}, {"relu-grad", OpCode::ReluGrad},
    {"matmul-grad-a", OpCode::MatMulGradA}, {"matmul-grad-b", OpCode::MatMulGradB},
    {"bias-add", OpCode::BiasAdd}, {"bias-add-grad", OpCode::BiasAddGrad},
    {"layernorm", OpCode::LayerNorm}, {"layernorm-grad", OpCode::LayerNormGrad},
    {"softmax-ce-loss", OpCode::SoftmaxCrossEntropyLoss}, {"softmax-ce-grad", OpCode::SoftmaxCrossEntropyGrad},
    {"adam-step", OpCode::AdamStep}, {"causal-attention", OpCode::CausalAttention}, {"causal-attention-grad", OpCode::CausalAttentionGrad},
    {"custom", OpCode::Custom}
  };
  for (auto& [n, c] : t) if (s == n) { out = c; return true; }
  return false;
}

void Operation::encode(Writer& w) const {
  w.id(id); w.u16((uint16_t)code);
  w.u8((uint8_t)requirement.device); w.u64(requirement.memory); w.u32(requirement.parallelism); w.u32(requirement.priority); w.u64(requirement.flops); w.id(requirement.affinity); w.str(requirement.device_selector);
  w.u32((uint32_t)inputs.size()); for (auto& t : inputs) t.encode(w);
  w.u32((uint32_t)outputs.size()); for (auto& t : outputs) t.encode(w);
  w.u32((uint32_t)depends.size()); for (auto& d : depends) w.id(d);
  w.f64(scalar); w.u64(custom_kernel);
}
Result<Operation> Operation::decode(Reader& r) {
  Operation op;
  op.id = OperationID{r.id()}; op.code = (OpCode)r.u16();
  op.requirement.device = (DeviceType)r.u8(); op.requirement.memory = r.u64(); op.requirement.parallelism = r.u32(); op.requirement.priority = r.u32(); op.requirement.flops = r.u64(); op.requirement.affinity = DeviceID{r.id()}; op.requirement.device_selector = r.str();
  uint32_t n = r.u32(); if (!r.ok() || n > 4096) return Result<Operation>(Status::Corrupt, "op inputs");
  for (uint32_t i = 0; i < n; ++i) { auto t = Tensor::decode(r); if (!t.ok()) return t.error(); op.inputs.push_back(*t); }
  n = r.u32(); if (!r.ok() || n > 4096) return Result<Operation>(Status::Corrupt, "op outputs");
  for (uint32_t i = 0; i < n; ++i) { auto t = Tensor::decode(r); if (!t.ok()) return t.error(); op.outputs.push_back(*t); }
  n = r.u32(); if (!r.ok() || n > 65536) return Result<Operation>(Status::Corrupt, "op deps");
  for (uint32_t i = 0; i < n; ++i) op.depends.push_back(OperationID{r.id()});
  op.scalar = r.f64(); op.custom_kernel = r.u64();
  if (!r.ok()) return Result<Operation>(Status::Corrupt, "operation record");
  return op;
}

const Operation* ComputeGraph::find(const OperationID& oid) const {
  for (auto& op : operations) if (op.id == oid) return &op;
  return nullptr;
}

Result<std::vector<OperationID>> ComputeGraph::order() const {
  // Data dependencies: an operation reading memory M depends on every earlier
  // (in declaration order) operation writing M; a writer depends on earlier
  // readers and writers of M (no reordering of hazards).
  std::map<OperationID, std::set<OperationID>> deps;
  std::map<MemoryID, std::vector<size_t>> writers, readers;
  for (size_t i = 0; i < operations.size(); ++i) {
    const auto& op = operations[i];
    auto& d = deps[op.id];
    for (auto& e : op.depends) d.insert(e);
    for (auto& t : op.inputs) for (size_t w : writers[t.memory]) d.insert(operations[w].id);
    for (auto& t : op.outputs) {
      for (size_t w : writers[t.memory]) d.insert(operations[w].id);
      for (size_t rd : readers[t.memory]) if (rd != i) d.insert(operations[rd].id);
    }
    for (auto& t : op.inputs) readers[t.memory].push_back(i);
    for (auto& t : op.outputs) writers[t.memory].push_back(i);
    d.erase(op.id);
  }
  std::vector<OperationID> out;
  std::set<OperationID> done;
  bool progress = true;
  while (out.size() < operations.size() && progress) {
    progress = false;
    for (auto& op : operations) {
      if (done.count(op.id)) continue;
      bool ready = true;
      for (auto& d : deps[op.id]) if (!done.count(d)) { if (!find(d)) return Result<std::vector<OperationID>>(Status::Invalid, "operation depends on unknown operation " + d.short_str()); ready = false; break; }
      if (ready) { out.push_back(op.id); done.insert(op.id); progress = true; }
    }
  }
  if (out.size() < operations.size()) return Result<std::vector<OperationID>>(Status::Invalid, "graph has a dependency cycle");
  return out;
}

uint64_t ComputeGraph::total_flops() const { uint64_t f = 0; for (auto& op : operations) f += op.requirement.flops; return f; }
uint64_t ComputeGraph::footprint() const {
  std::set<MemoryID> seen; uint64_t b = 0;
  for (auto& op : operations) { for (auto& t : op.inputs) if (seen.insert(t.memory).second) b += t.bytes(); for (auto& t : op.outputs) if (seen.insert(t.memory).second) b += t.bytes(); }
  return b;
}

void ComputeGraph::encode(Writer& w) const {
  w.id(id); w.id(owner); w.u64(created_ns);
  w.u32((uint32_t)operations.size()); for (auto& op : operations) op.encode(w);
}
Result<ComputeGraph> ComputeGraph::decode(Reader& r) {
  ComputeGraph g;
  g.id = GraphID{r.id()}; g.owner = EntityID{r.id()}; g.created_ns = r.u64();
  uint32_t n = r.u32(); if (!r.ok() || n > (1u << 20)) return Result<ComputeGraph>(Status::Corrupt, "graph ops");
  for (uint32_t i = 0; i < n; ++i) { auto op = Operation::decode(r); if (!op.ok()) return op.error(); g.operations.push_back(*op); }
  return g;
}

GraphStore::GraphStore(IdGenerator& ids) : ids_(ids) {}
Result<GraphID> GraphStore::create(const EntityID& owner, uint64_t now_ns) {
  ComputeGraph g; g.id = ids_.next_as<GraphID>(); g.owner = owner; g.created_ns = now_ns;
  GraphID id = g.id;
  hal::LockGuard l(mu_);
  graphs_[id] = std::move(g);
  return id;
}
Result<GraphID> GraphStore::install(const ComputeGraph& g) {
  hal::LockGuard l(mu_);
  if (graphs_.count(g.id)) return Result<GraphID>(Status::Conflict, "graph identity already present");
  graphs_[g.id] = g;
  return g.id;
}
Result<OperationID> GraphStore::add(const GraphID& gid, Operation op) {
  hal::LockGuard l(mu_);
  auto it = graphs_.find(gid);
  if (it == graphs_.end()) return Result<OperationID>(Status::NotFound, "graph " + gid.short_str());
  if (op.id.is_null()) op.id = ids_.next_as<OperationID>();
  OperationID id = op.id;
  it->second.operations.push_back(std::move(op));
  return id;
}
Result<ComputeGraph> GraphStore::copy(const GraphID& gid) const {
  hal::LockGuard l(mu_);
  auto it = graphs_.find(gid);
  if (it == graphs_.end()) return Result<ComputeGraph>(Status::NotFound, "graph " + gid.short_str());
  return it->second;
}
const ComputeGraph* GraphStore::get(const GraphID& gid) const {
  hal::LockGuard l(mu_);
  auto it = graphs_.find(gid);
  return it == graphs_.end() ? nullptr : &it->second;
}
Result<void> GraphStore::destroy(const GraphID& gid) {
  hal::LockGuard l(mu_);
  if (!graphs_.erase(gid)) return Result<void>(Status::NotFound, "graph " + gid.short_str());
  return Ok();
}
Result<void> GraphStore::destroy_owned(const EntityID& owner) {
  hal::LockGuard l(mu_);
  for (auto it = graphs_.begin(); it != graphs_.end();) { if (it->second.owner == owner) it = graphs_.erase(it); else ++it; }
  return Ok();
}
std::vector<ComputeGraph> GraphStore::list(const EntityID& owner) const {
  hal::LockGuard l(mu_);
  std::vector<ComputeGraph> out;
  for (auto& [id, g] : graphs_) if (owner.is_null() || g.owner == owner) out.push_back(g);
  return out;
}

}  // namespace aios
