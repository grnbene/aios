#include "aios/compute/scheduler.hpp"
#include "aios/hal/sync.hpp"
#include "aios/hal/lane_pool.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <map>

namespace aios {

Scheduler::Scheduler(hal::Host& host, DeviceFabric& devices, MemoryFabric& memory, CapabilityManager& caps,
                     PreemptiveSchedulerQuota& quotas)
    : host_(host), devices_(devices), memory_(memory), caps_(caps), quotas_(quotas) {}

namespace {
uint64_t footprint_of(const Operation& op) {
  uint64_t f = op.requirement.memory;
  if (!f) for (auto* list : {&op.inputs, &op.outputs}) for (auto& t : *list) f += t.bytes();
  return f;
}

// Arithmetic an operation does, where it is more than touching each element
// once (that part the per-byte cost already carries).
double flops_of(const Operation& op) {
  auto dim = [&](const std::vector<Tensor>& v, size_t i, size_t d) -> double {
    return i < v.size() && d < v[i].shape.size() ? double(v[i].shape[d]) : 0.0;
  };
  switch (op.code) {
    case OpCode::MatMul: return 2.0 * dim(op.inputs, 0, 0) * dim(op.inputs, 0, 1) * dim(op.inputs, 1, 1);
    case OpCode::MatMulGradA: return 2.0 * dim(op.inputs, 0, 0) * dim(op.inputs, 0, 1) * dim(op.inputs, 1, 0);
    case OpCode::MatMulGradB: return 2.0 * dim(op.inputs, 0, 0) * dim(op.inputs, 0, 1) * dim(op.inputs, 1, 1);
    case OpCode::CausalAttention:
    case OpCode::CausalAttentionGrad: {
      const double t = dim(op.inputs, 0, 0), d = dim(op.inputs, 0, 1);
      return (op.code == OpCode::CausalAttention ? 2.0 : 4.0) * t * t * d;
    }
    default: return 0.0;
  }
}
uint64_t operand_bytes(const Operation& op) {
  uint64_t b = 0;
  for (auto* list : {&op.inputs, &op.outputs}) for (auto& t : *list) b += t.bytes();
  return b;
}

// The work an operation does, for scaling one measured size of its kind to
// another: bytes it touches plus the arithmetic beyond one pass over them.
double work_of(const Operation& op) { return double(operand_bytes(op)) + 0.25 * flops_of(op) + 1.0; }

// A scratch copy of an operation: the same kind, shapes and scalar over fresh
// system memory filled with 0.5 (valid for every kind - as an index it is 0).
// The graph's own tensors are never touched.
struct Scratch {
  MemoryFabric& mem;
  Operation o;
  std::vector<MemoryID> held;
  bool ok = true;
  Scratch(MemoryFabric& m, const EntityID& owner, const Operation& op) : mem(m) {
    o.code = op.code;
    o.scalar = op.scalar;
    o.custom_kernel = op.custom_kernel;
    o.requirement.parallelism = op.requirement.parallelism;
    auto clone = [&](const std::vector<Tensor>& from, std::vector<Tensor>* into) {
      for (const Tensor& t0 : from) {
        Tensor t;
        t.dtype = t0.dtype;
        t.shape = t0.shape;
        auto r = mem.allocate(owner, std::max<uint64_t>(t.bytes(), 4));
        if (!r.ok()) { ok = false; return; }
        held.push_back(*r);
        t.memory = *r;
        if (t.dtype == DType::F32) {
          std::vector<float> fill(t.elements(), 0.5f);
          if (!mem.write(*r, 0, ByteSpan{reinterpret_cast<const uint8_t*>(fill.data()), t.bytes()}).ok()) ok = false;
        }
        into->push_back(t);
      }
    };
    clone(op.inputs, &o.inputs);
    clone(op.outputs, &o.outputs);
  }
  ~Scratch() { for (auto& m : held) mem.release(m); }
};
int size_class(double work) { int c = 0; while (work >= 4.0) { work /= 4.0; ++c; } return c; }
}  // namespace

// Best of three after a warm-up; -1 when the device cannot run it. A warm-up
// already past give_up_after_ns is the answer (not worth timing again).
double Scheduler::time_op(Device* d, const Operation& o, double give_up_after_ns) const {
  if (!d->supports(o)) return -1;
  const uint32_t lanes = std::max<uint32_t>(1, std::min(o.requirement.parallelism ? o.requirement.parallelism : 1u, d->capacity().lanes));
  double best = -1;
  for (int i = 0; i < 4; ++i) {
    const uint64_t t0 = host_.monotonic_ns();
    if (!d->execute(o, memory_, lanes).ok()) return -1;
    const double ns = double(host_.monotonic_ns() - t0);
    if (i == 0) { if (give_up_after_ns > 0 && ns > give_up_after_ns) return ns; continue; }
    if (best < 0 || ns < best) best = ns;
  }
  return best;
}

double Scheduler::overhead_ns(Device* d, const EntityID& owner) const {
  auto it = overhead_.find(d->id());
  if (it != overhead_.end()) return it->second;
  Operation probe;
  probe.code = OpCode::Add;
  Tensor t;
  t.dtype = DType::F32;
  t.shape = {16};
  probe.inputs = {t, t};
  probe.outputs = {t};
  Scratch s(memory_, owner, probe);
  const double ns = s.ok ? time_op(d, s.o, 0) : -1;
  return overhead_[d->id()] = ns < 0 ? 0 : ns;
}

double Scheduler::predict_ns(const Operation& op, const std::vector<Device*>& cands, size_t which, const EntityID& owner) const {
  hal::LockGuard g(cost_mu_);
  const uint16_t kind = (uint16_t)op.code;
  const int cls = size_class(work_of(op));
  auto key = [&](Device* d) { return std::make_tuple(d->id(), kind, cls); };
  bool known = true;
  for (Device* d : cands) known &= kind_costs_.count(key(d)) > 0;
  if (!known) {
    Scratch s(memory_, owner, op);
    // the CPU first: cheap to time, and it bounds what the others may cost
    std::vector<size_t> order(cands.size());
    for (size_t i = 0; i < cands.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      return (cands[a]->type() == DeviceType::CPU) > (cands[b]->type() == DeviceType::CPU);
    });
    double best_seen = -1;
    for (size_t idx : order) {
      Device* d = cands[idx];
      if (kind_costs_.count(key(d))) { const auto& k = kind_costs_[key(d)]; if (k.ns > 0 && (best_seen < 0 || k.ns < best_seen)) best_seen = k.ns; continue; }
      KindCost kc;
      kc.work = work_of(op);
      const double oh = best_seen > 0 ? overhead_ns(d, owner) : 0;
      if (!s.ok) kc.ns = -1;
      else if (best_seen > 0 && oh >= best_seen) kc.ns = oh + best_seen;   // cannot win: its fixed cost alone is more
      else kc.ns = time_op(d, s.o, best_seen > 0 ? 20.0 * best_seen : 0);
      if (kc.ns > 0 && (best_seen < 0 || kc.ns < best_seen)) best_seen = kc.ns;
      kind_costs_[key(d)] = kc;
    }
  }
  const KindCost& kc = kind_costs_[key(cands[which])];
  if (kc.ns < 0) return -1;
  return kc.ns * (work_of(op) / kc.work);
}

Result<DeviceMemoryStats> Scheduler::memory_stats_of(Device* d) const {
  const uint64_t now = host_.monotonic_ns();
  {
    hal::LockGuard g(mem_mu_);
    auto it = mem_stats_.find(d->id());
    if (it != mem_stats_.end() && now - it->second.first < 100000000ull) return it->second.second;
  }
  auto ms = d->memory_stats();
  if (!ms.ok()) return ms;
  hal::LockGuard g(mem_mu_);
  mem_stats_[d->id()] = {now, *ms};
  return ms;
}

Result<Placement> Scheduler::place(const Operation& op, const EntityID& owner) const {
  uint64_t now = host_.wall_ns();
  std::vector<Device*> candidates;
  // A named requirement is resolved against THIS kernel's fabric, every time
  // the operation is placed. That is what lets a half-finished graph carry on
  // in a kernel that has different device instances — or better ones.
  if (!op.requirement.device_selector.empty()) {
    const std::string& want = op.requirement.device_selector;
    for (Device* d : devices_.all()) {
      if (d->name().rfind(want, 0) == 0 || d->id().short_str().rfind(want, 0) == 0) candidates.push_back(d);
    }
    if (candidates.empty()) return Result<Placement>(Status::NotFound, "no device answering to '" + want + "' in this kernel's fabric");
  } else if (!op.requirement.affinity.is_null()) {
    Device* d = devices_.get(op.requirement.affinity);
    if (!d) return Result<Placement>(Status::NotFound, "pinned device " + op.requirement.affinity.short_str() + " is not in the fabric");
    candidates.push_back(d);
  } else candidates = devices_.of_type(op.requirement.device);
  if (candidates.empty()) return Result<Placement>(Status::Unsupported, std::string("no ") + device_type_name(op.requirement.device) + " device in the fabric");
  Device* best = nullptr;
  double best_score = -1e300;
  std::vector<std::pair<Device*, double>> passing;
  std::string reason;
  size_t capable = 0, permitted = 0, roomy = 0;
  for (Device* d : candidates) {
    if (!d->supports(op)) continue;
    ++capable;
    // Capability: the owner must hold Execute over this device or the device class.
    if (!caps_.check(owner, ResourceRef{ResourceKind::ComputeDevice, d->id()}, R_Execute, now)) continue;
    ++permitted;
    const auto& cap = d->capacity();
    if (op.requirement.memory && cap.memory && op.requirement.memory > cap.memory) continue;
    // What the device has FREE right now, not what it was built with: a card
    // another process has filled with a training run is not a candidate for
    // an operation whose operands will not fit beside it, and a half-full one
    // is a worse choice than an idle one of the same kind.
    uint64_t footprint = op.requirement.memory;
    if (!footprint) for (auto* list : {&op.inputs, &op.outputs}) for (auto& t : *list) footprint += t.bytes();
    double free_fraction = 1.0;
    if (auto ms = memory_stats_of(d); ms.ok() && ms->total_bytes) {
      if (!ms->source.empty() && footprint > ms->free_bytes) continue;
      free_fraction = double(ms->free_bytes) / double(ms->total_bytes);
    }
    ++roomy;
    // Locality: bytes already resident in the device's native memory type.
    // Locality counts what WILL be local when the operation runs: execute()
    // migrates system-memory operands to the device's native type whenever a
    // provider for it exists, so scoring them as foreign here handed every
    // graph to the CPU and left an idle GPU unplaced for host-resident data.
    uint64_t local = 0, total = 0;
    const bool can_migrate = cap.reads_host_memory || memory_.has_provider(cap.native_memory);
    for (auto* list : {&op.inputs, &op.outputs}) for (auto& t : *list) {
      const MemoryObject* m = memory_.lookup(t.memory);
      if (!m) continue;
      total += t.bytes();
      if (m->type == cap.native_memory) local += t.bytes();
      else if (can_migrate && m->type != MemoryType::Persistent && m->type != MemoryType::Shared) local += t.bytes();
    }
    double locality = total ? double(local) / double(total) : 1.0;
    double free_lanes = cap.lanes ? double(cap.lanes - std::min(cap.lanes, d->busy_lanes())) / double(cap.lanes) : 0.0;
    double throughput = cap.flops_per_second ? std::log2(1.0 + double(cap.flops_per_second) / 1e9) : 0.0;
    double score = locality * 10.0 + free_lanes * 3.0 + free_fraction * 2.0 + throughput;
    passing.push_back({d, score});
    if (score > best_score) {
      best_score = score; best = d;
      reason = "locality=" + std::to_string(int(locality * 100)) + "% free-lanes=" + std::to_string(int(free_lanes * 100)) +
               "% free-memory=" + std::to_string(int(free_fraction * 100)) + "%";
    }
  }
  // More than one device could run it: the one it will finish soonest on, by
  // each device's measured costs. (The score above stays the answer when any
  // candidate could not be measured - a device without Add or MatMul.) A GPU
  // that must receive, launch, synchronise and return for every operation is
  // the wrong place for a 256-element add; the score, which counted system
  // memory as the GPU's own, sent every one of a training step's 2130
  // operations there - 12x slower than the CPU.
  if (passing.size() > 1) {
    bool all = true;
    std::vector<double> est;
    std::vector<Device*> cands;
    for (auto& pc : passing) cands.push_back(pc.first);
    for (size_t i = 0; i < cands.size(); ++i) {
      est.push_back(predict_ns(op, cands, i, owner));
      all &= est.back() >= 0;
    }
    if (all) {
      size_t k = 0;
      for (size_t i = 1; i < passing.size(); ++i) if (est[i] < est[k]) k = i;
      best = passing[k].first;
      reason = "estimated " + std::to_string(est[k] / 1000.0).substr(0, 7) + " us (";
      for (size_t i = 0; i < passing.size(); ++i)
        reason += (i ? ", " : "") + passing[i].first->name().substr(0, 12) + " " + std::to_string(est[i] / 1000.0).substr(0, 7);
      reason += " us)";
    }
  }
  if (!best) {
    // Say which gate closed: the wrong one sends an operator to the authority
    // ledger when the device simply has no kernel for the operation.
    const std::string what = std::string(opcode_name(op.code)) + (op.requirement.device_selector.empty() ? "" : " on '" + op.requirement.device_selector + "'");
    if (!capable) return Result<Placement>(Status::Unsupported, "no candidate device has a kernel for " + what + " (" + std::to_string(candidates.size()) + " candidate(s))");
    if (!permitted) return Result<Placement>(Status::Denied, "entity " + owner.short_str() + " holds no execute capability over any of the " + std::to_string(capable) + " device(s) that can run " + what);
    if (!roomy) return Result<Placement>(Status::Exhausted, "every device that can run " + what + " is too full for its " + std::to_string(footprint_of(op)) + " byte(s) of operands right now");
    return Result<Placement>(Status::Unsupported, "no device could be placed for " + what);
  }
  Placement p;
  p.op = op.id; p.device = best->id(); p.device_name = best->name();
  p.lanes = std::max<uint32_t>(1, std::min(op.requirement.parallelism, best->capacity().lanes));
  p.reason = reason;
  return p;
}

Result<std::vector<Placement>> Scheduler::plan(const ComputeGraph& g, const EntityID& owner) const {
  auto order = g.order();
  if (!order.ok()) return order.error();
  std::vector<Placement> out;
  for (auto& oid : *order) {
    auto p = place(*g.find(oid), owner);
    if (!p.ok()) return p.error();
    out.push_back(*p);
  }
  return out;
}

Result<ExecutionReport> Scheduler::execute(const ComputeGraph& g, const EntityID& owner, uint64_t start_position, const SchedulePolicy& policy) {
  ExecutionReport rep;
  rep.graph = g.id;
  rep.operations_total = g.operations.size();
  rep.position = start_position;
  uint64_t t0 = host_.monotonic_ns();
  uint64_t now = host_.wall_ns();
  AIOS_TRY(caps_.require(owner, ResourceRef{ResourceKind::Kernel, {}}, R_Schedule, now));
  auto order = g.order();
  if (!order.ok()) return order.error();
  if (start_position > order->size()) return Result<ExecutionReport>(Status::Invalid, "execution position beyond graph");

  // Dependency map (same rules as ComputeGraph::order) for wave construction.
  std::map<OperationID, std::set<OperationID>> deps;
  {
    std::map<MemoryID, std::vector<size_t>> writers, readers;
    for (size_t i = 0; i < g.operations.size(); ++i) {
      const auto& op = g.operations[i];
      auto& d = deps[op.id];
      for (auto& e : op.depends) d.insert(e);
      for (auto& t : op.inputs) for (size_t w : writers[t.memory]) d.insert(g.operations[w].id);
      for (auto& t : op.outputs) { for (size_t w : writers[t.memory]) d.insert(g.operations[w].id); for (size_t rd : readers[t.memory]) if (rd != i) d.insert(g.operations[rd].id); }
      for (auto& t : op.inputs) readers[t.memory].push_back(i);
      for (auto& t : op.outputs) writers[t.memory].push_back(i);
      d.erase(op.id);
    }
  }
  std::set<OperationID> done(order->begin(), order->begin() + start_position);
  uint64_t budget = policy.max_operations ? policy.max_operations : UINT64_MAX;
  uint32_t wave = 0;
  size_t cursor = start_position;
  // An entity that has already spent its window does not get to start a wave.
  if (!quotas_.may_run(owner, host_.monotonic_ns())) {
    rep.preempted = true;
    rep.position = cursor;
    rep.elapsed_ns = host_.monotonic_ns() - t0;
    return rep;
  }
  uint64_t slice_ns = quotas_.runnable_ns(owner, host_.monotonic_ns());
  while (cursor < order->size() && budget > 0) {
    // Ready set: everything whose deps are done, highest priority first.
    std::vector<const Operation*> ready;
    for (size_t i = cursor; i < order->size(); ++i) {
      const Operation* op = g.find((*order)[i]);
      if (done.count(op->id)) continue;
      bool ok = true;
      for (auto& d : deps[op->id]) if (!done.count(d)) { ok = false; break; }
      if (ok) ready.push_back(op);
    }
    if (ready.empty()) { rep.failure = "no ready operation (cycle?)"; break; }
    std::stable_sort(ready.begin(), ready.end(), [](const Operation* a, const Operation* b) { return a->requirement.priority > b->requirement.priority; });
    if (ready.size() > budget) ready.resize((size_t)budget);

    // Place, then fit the wave into available lanes; overflow rolls to the next wave.
    std::vector<std::pair<const Operation*, Placement>> wave_ops;
    std::map<DeviceID, uint32_t> lanes_used;
    for (const Operation* op : ready) {
      auto p = place(*op, owner);
      if (!p.ok()) { rep.failure = p.error().str(); rep.elapsed_ns = host_.monotonic_ns() - t0; return rep; }
      Device* dev = devices_.get(p->device);
      uint32_t& used = lanes_used[p->device];
      if (used + p->lanes > dev->capacity().lanes && !wave_ops.empty()) continue;  // next wave
      // Memory locality: pull operands toward the device's native memory type.
      if (policy.allow_migration) {
        for (auto* list : {&op->inputs, &op->outputs}) for (auto& t : *list) {
          const MemoryObject* m = memory_.lookup(t.memory);
          if (m && !dev->capacity().reads_host_memory && m->type != dev->capacity().native_memory && m->type != MemoryType::Persistent && m->type != MemoryType::Shared && memory_.has_provider(dev->capacity().native_memory)) {
            if (memory_.migrate(t.memory, dev->capacity().native_memory).ok()) p->migrated_bytes += m->size;
          }
        }
      }
      used += p->lanes;
      p->wave = wave;
      wave_ops.push_back({op, *p});
    }
    // Run the wave concurrently on the process's worker pool (a wave of one
    // runs here, where its own slices can still use every worker).
    std::vector<Result<void>> results(wave_ops.size(), Ok());
    std::vector<ExecutionSample> samples(wave_ops.size());
    hal::run_tasks((uint32_t)wave_ops.size(), [&](uint32_t i) {
      const Operation* op = wave_ops[i].first; Placement& p = wave_ops[i].second;
      Device* dev = devices_.get(p.device);
      samples[i].op = op->id; samples[i].device = p.device; samples[i].lanes_used = p.lanes;
      samples[i].started_ns = host_.monotonic_ns();
      results[i] = dev->execute(*op, memory_, p.lanes);
      samples[i].finished_ns = host_.monotonic_ns();
    });
    for (size_t i = 0; i < wave_ops.size(); ++i) {
      rep.placements.push_back(wave_ops[i].second);
      rep.samples.push_back(samples[i]);
      if (!results[i].ok()) {
        rep.failure = std::string(opcode_name(wave_ops[i].first->code)) + " " + wave_ops[i].first->id.short_str() + ": " + results[i].error().str();
        rep.elapsed_ns = host_.monotonic_ns() - t0;
        return rep;
      }
      done.insert(wave_ops[i].first->id);
      rep.operations_run++;
      budget--;
    }
    // Charge the owner for what the devices actually spent. A wave runs its
    // operations at the same time, so it costs the longest of them, not the
    // sum: charging the sum would bill an entity for parallelism it was given
    // rather than for time it held the machine.
    uint64_t wave_ns = 0;
    for (size_t i = rep.samples.size() - wave_ops.size(); i < rep.samples.size(); ++i) {
      const auto& smp = rep.samples[i];
      if (smp.finished_ns > smp.started_ns) wave_ns = std::max(wave_ns, smp.finished_ns - smp.started_ns);
    }
    rep.charged_ns += wave_ns;
    bool within_budget = quotas_.charge(owner, host_.monotonic_ns(), wave_ns, wave_ops.size());

    // Advance the linear position over the completed prefix of the order.
    while (cursor < order->size() && done.count((*order)[cursor])) cursor++;
    // Operations completed out of prefix order still count; the position is
    // the completed prefix, which is what a resume can rely on.
    wave++;
    if (!within_budget) { rep.preempted = true; break; }
    if (slice_ns != UINT64_MAX && rep.charged_ns >= slice_ns) { rep.preempted = true; break; }
  }
  rep.position = cursor;
  rep.complete = cursor == order->size();
  if (rep.complete) rep.preempted = false;
  rep.elapsed_ns = host_.monotonic_ns() - t0;
  return rep;
}

}  // namespace aios
