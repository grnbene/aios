#pragma once
// The kernel schedules operations, not applications. A ComputeGraph is a DAG
// of Operations over Tensors; each Operation declares what it requires and the
// scheduler decides where, when and how it runs.
#include <vector>
#include "aios/hal/sync.hpp"
#include <string>
#include <unordered_map>
#include "aios/core/ids.hpp"
#include "aios/core/result.hpp"
#include "aios/core/bytes.hpp"
#include "aios/memory/tensor.hpp"

namespace aios {

enum class DeviceType : uint8_t { Any = 0, CPU = 1, GPU = 2, NPU = 3, RemoteCPU = 4, RemoteGPU = 5, RemoteNPU = 6 };
const char* device_type_name(DeviceType t);
bool parse_device_type(const std::string& s, DeviceType& out);

struct ComputeRequirement {
  DeviceType device = DeviceType::Any;
  uint64_t memory = 0;        // bytes the operation needs resident
  uint32_t parallelism = 1;   // lanes the operation can use
  uint32_t priority = 100;    // higher runs first among ready operations
  uint64_t flops = 0;         // estimate; scheduler uses it for placement
  // What the operation NEEDS, not where it ran: a device name or class the
  // scheduler resolves wherever the graph is executing. A resolved device is
  // an instance of one kernel's fabric, so storing one in the requirement
  // makes a graph unrunnable the moment it moves to another kernel — which is
  // precisely what a checkpointed, movable computation does.
  std::string device_selector;
  DeviceID affinity;          // an exact instance, for a caller that means THIS device in THIS kernel
};

enum class OpCode : uint16_t {
  Nop = 0,
  Copy = 1,        // out = in0
  Fill = 2,        // out = scalar
  Add = 3,         // out = in0 + in1
  Mul = 4,         // out = in0 * in1
  Scale = 5,       // out = in0 * scalar
  MatMul = 6,      // out[MxN] = in0[MxK] · in1[KxN]
  ReduceSum = 7,   // out[1] = sum(in0)
  Relu = 8,
  Softmax = 9,     // over the last axis
  Histogram = 10,  // out[256] f32 = byte histogram of in0 (u8)
  Normalize = 11,  // out = in0 / sum(in0)
  Gelu = 12,       // out = gelu(in0)
  GeluGrad = 13,   // out = in0_grad * dgelu(in1_pre)   (dy first, then the pre-activation)
  ReluGrad = 14,   // out = (in1_pre > 0 ? in0_grad : 0)
  MatMulGradA = 15,// out[MxK] = dC[MxN] · B[KxN]^T
  MatMulGradB = 16,// out[KxN] = A[MxK]^T · dC[MxN]
  BiasAdd = 17,    // out[MxN] = in0[MxN] + bias[N]
  BiasAddGrad = 18,// out[N] = sum_rows(dC[MxN])
  LayerNorm = 19,  // out0[MxN]=norm(in0[MxN], gamma[N], beta[N]), out1[M]=mu, out2[M]=inv_std
  LayerNormGrad = 20, // out0[MxN]=dx(dY[MxN], X[MxN], gamma[N]), out1[N]=dgamma, out2[N]=dbeta
  SoftmaxCrossEntropyLoss = 21, // out0[1]=loss, out1[MxN]=probs from logits[MxN] and targets[M]
  SoftmaxCrossEntropyGrad = 22, // out0[MxN]=dlogits from probs[MxN] and targets[M]
  AdamStep = 23,   // updates weight in0, given grad in1, m in2, v in3; scalar holds lr
  CausalAttention = 24,     // in q,k,v [TxD]; scalar = heads H; out0 o[TxD], out1 att[HxTxT] (post-softmax, causal)
  CausalAttentionGrad = 25, // in dO[TxD], q, k, v, att; scalar = H; out dq, dk, dv [TxD]
  Custom = 100,    // kernel registered on a device under `custom_kernel`
};
const char* opcode_name(OpCode c);
bool parse_opcode(const std::string& s, OpCode& out);

struct Operation {
  OperationID id;
  OpCode code = OpCode::Nop;
  ComputeRequirement requirement;
  std::vector<Tensor> inputs;
  std::vector<Tensor> outputs;
  std::vector<OperationID> depends;   // explicit dependencies; data deps are inferred too
  double scalar = 0.0;
  uint64_t custom_kernel = 0;         // fnv1a of a registered kernel name
  // Outputs (by index, 1 = leave) a REMOTE device keeps where they were made
  // instead of reading them back: an operation after this one on the same
  // device that takes the buffer as input finds it already there, so a
  // result (a proof) is used where it was produced and never crosses the
  // link. The local copy is not updated. A runtime hint - not on the wire.
  std::vector<uint8_t> leave_remote;

  void encode(Writer& w) const;
  static Result<Operation> decode(Reader& r);
};

struct ComputeGraph {
  GraphID id;
  EntityID owner;
  uint64_t created_ns = 0;
  std::vector<Operation> operations;

  const Operation* find(const OperationID& id) const;
  // Dependency order honoring explicit deps and write→read data dependencies.
  Result<std::vector<OperationID>> order() const;
  uint64_t total_flops() const;
  uint64_t footprint() const;   // bytes touched

  void encode(Writer& w) const;
  static Result<ComputeGraph> decode(Reader& r);
};

class GraphStore {
 public:
  explicit GraphStore(IdGenerator& ids);
  Result<GraphID> create(const EntityID& owner, uint64_t now_ns);
  Result<GraphID> install(const ComputeGraph& g);
  Result<OperationID> add(const GraphID& g, Operation op);
  Result<ComputeGraph> copy(const GraphID& g) const;
  const ComputeGraph* get(const GraphID& g) const;
  Result<void> destroy(const GraphID& g);
  Result<void> destroy_owned(const EntityID& owner);
  std::vector<ComputeGraph> list(const EntityID& owner = {}) const;
 private:
  IdGenerator& ids_;
  mutable hal::Mutex mu_;
  std::unordered_map<GraphID, ComputeGraph, TypedIdHash<7>> graphs_;
};

}  // namespace aios
