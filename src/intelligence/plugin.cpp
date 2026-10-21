// Plugin intelligences: a shared object exporting aios_intelligence_create is
// adapted onto the C++ Intelligence interface, and the kernel Surface is
// projected onto the C surface table. dlopen is host-specific and lives here,
// outside the kernel proper.
#include "aios/intelligence/intelligence.hpp"
#include "aios/intelligence/abi.h"
#include "aios/kernel/surface.hpp"
#include <cstring>

struct aios_surface { aios::Surface* s; };

namespace aios {
namespace {

int to_code(Status st) { return (int)st; }
inline aios_id to_c(const Id128& id) { return aios_id{id.hi, id.lo}; }
inline Id128 from_c(aios_id id) { return Id128{id.hi, id.lo}; }

int c_mem_alloc(aios_surface* s, uint64_t size, aios_id* out) { auto r = s->s->allocate(size); if (!r.ok()) return to_code(r.status()); *out = to_c(*r); return 0; }
int c_mem_write(aios_surface* s, aios_id mem, uint64_t off, const void* data, uint64_t n) { auto r = s->s->write(MemoryID{from_c(mem)}, off, ByteSpan((const uint8_t*)data, n)); return to_code(r.status()); }
int c_mem_read(aios_surface* s, aios_id mem, uint64_t off, void* data, uint64_t n) { auto r = s->s->read(MemoryID{from_c(mem)}, off, n); if (!r.ok()) return to_code(r.status()); std::memcpy(data, r->data(), std::min<uint64_t>(n, r->size())); return 0; }
int c_mem_size(aios_surface* s, aios_id mem, uint64_t* out) { auto r = s->s->describe(MemoryID{from_c(mem)}); if (!r.ok()) return to_code(r.status()); *out = (*r)->size; return 0; }
int c_state_create(aios_surface* s, uint8_t kind, const void* payload, uint64_t n, aios_id* out) { auto r = s->s->create_state((StateKind)kind, ByteSpan((const uint8_t*)payload, n)); if (!r.ok()) return to_code(r.status()); *out = to_c(*r); return 0; }
int c_state_payload(aios_surface* s, aios_id st, void* buf, uint64_t cap, uint64_t* full) { auto r = s->s->state(StateID{from_c(st)}); if (!r.ok()) return to_code(r.status()); *full = r->payload.size(); if (buf && cap) std::memcpy(buf, r->payload.data(), std::min<uint64_t>(cap, r->payload.size())); return 0; }
int c_state_set_payload(aios_surface* s, aios_id st, const void* payload, uint64_t n) { return to_code(s->s->set_state(StateID{from_c(st)}, ByteSpan((const uint8_t*)payload, n)).status()); }
int c_state_attach_memory(aios_surface* s, aios_id st, aios_id mem) { return to_code(s->s->attach(StateID{from_c(st)}, MemoryID{from_c(mem)}).status()); }
int c_state_memory(aios_surface* s, aios_id st, aios_id* out, uint32_t cap, uint32_t* count) { auto r = s->s->state(StateID{from_c(st)}); if (!r.ok()) return to_code(r.status()); *count = (uint32_t)r->memory.size(); for (uint32_t i = 0; i < *count && i < cap; ++i) out[i] = to_c(r->memory[i]); return 0; }
int c_graph_create(aios_surface* s, aios_id* out) { auto r = s->s->create_graph(); if (!r.ok()) return to_code(r.status()); *out = to_c(*r); return 0; }
int c_graph_add_op(aios_surface* s, aios_id g, const aios_op* cop, aios_id* out) {
  Operation op;
  op.code = (OpCode)cop->code;
  op.requirement.device = (DeviceType)cop->device; op.requirement.parallelism = cop->parallelism ? cop->parallelism : 1; op.requirement.priority = cop->priority ? cop->priority : 100;
  op.requirement.memory = cop->memory; op.requirement.flops = cop->flops; op.scalar = cop->scalar;
  auto conv = [](const aios_tensor& ct) { Tensor t; t.memory = MemoryID{from_c(ct.memory)}; t.dtype = (DType)ct.dtype; t.offset = ct.offset; for (uint32_t i = 0; i < ct.rank && i < 8; ++i) t.shape.push_back(ct.shape[i]); return t; };
  for (uint32_t i = 0; i < cop->n_inputs; ++i) op.inputs.push_back(conv(cop->inputs[i]));
  for (uint32_t i = 0; i < cop->n_outputs; ++i) op.outputs.push_back(conv(cop->outputs[i]));
  for (uint32_t i = 0; i < cop->n_depends; ++i) op.depends.push_back(OperationID{from_c(cop->depends[i])});
  auto r = s->s->add_operation(GraphID{from_c(g)}, std::move(op));
  if (!r.ok()) return to_code(r.status());
  *out = to_c(*r);
  return 0;
}
uint64_t c_now(aios_surface* s) { return s->s->now(); }
aios_id c_self(aios_surface* s) { return to_c(s->s->self()); }

const aios_surface_ops SURFACE_OPS = {c_mem_alloc, c_mem_write, c_mem_read, c_mem_size, c_state_create, c_state_payload, c_state_set_payload, c_state_attach_memory, c_state_memory, c_graph_create, c_graph_add_op, c_now, c_self};

class PluginIntelligence final : public Intelligence {
 public:
  PluginIntelligence(void* handle, aios_intelligence* mind, std::string path) : handle_(handle), mind_(mind), path_(std::move(path)) {}
  ~PluginIntelligence() override {
    if (mind_ && mind_->destroy) mind_->destroy(mind_);
    if (handle_) hal::unload_library(handle_);
  }
  std::string name() const override { return std::string("plugin:") + (mind_->name ? mind_->name : "?"); }
  Result<StateID> perceive(Surface& s, const StateObject& context) override {
    aios_surface cs{&s}; aios_id out{0, 0};
    int rc = mind_->perceive(mind_->ctx, &cs, &SURFACE_OPS, to_c(context.id), &out);
    if (rc) return Result<StateID>((Status)rc, "plugin perceive failed");
    return StateID{from_c(out)};
  }
  Result<GraphID> think(Surface& s, const StateObject& perceived) override {
    aios_surface cs{&s}; aios_id out{0, 0};
    int rc = mind_->think(mind_->ctx, &cs, &SURFACE_OPS, to_c(perceived.id), &out);
    if (rc) return Result<GraphID>((Status)rc, "plugin think failed");
    return GraphID{from_c(out)};
  }
  Result<StateID> act(Surface& s, const StateObject& perceived, const ComputeGraph* g, const ExecutionReport* rep) override {
    aios_surface cs{&s}; aios_id out{0, 0};
    int rc = mind_->act(mind_->ctx, &cs, &SURFACE_OPS, to_c(perceived.id), g ? to_c(g->id) : aios_id{0, 0}, rep ? rep->operations_run : 0, rep ? (rep->complete ? 1 : 0) : 0, &out);
    if (rc) return Result<StateID>((Status)rc, "plugin act failed");
    return StateID{from_c(out)};
  }
 private:
  void* handle_;
  aios_intelligence* mind_;
  std::string path_;
};

}  // namespace

Result<std::unique_ptr<Intelligence>> load_intelligence_plugin(const std::string& path) {
  void* h = hal::load_library(path);
  if (!h) return Result<std::unique_ptr<Intelligence>>(Status::IoError, "load plugin: " + hal::library_error());
  auto create = reinterpret_cast<aios_intelligence_create_fn>(hal::library_symbol(h, AIOS_INTELLIGENCE_ENTRY));
  if (!create) { hal::unload_library(h); return Result<std::unique_ptr<Intelligence>>(Status::Invalid, path + " does not export " AIOS_INTELLIGENCE_ENTRY); }
  aios_intelligence* mind = create();
  if (!mind) { hal::unload_library(h); return Result<std::unique_ptr<Intelligence>>(Status::IoError, "plugin returned no intelligence"); }
  if (mind->abi_version != AIOS_ABI_VERSION) { if (mind->destroy) mind->destroy(mind); hal::unload_library(h); return Result<std::unique_ptr<Intelligence>>(Status::Unsupported, "plugin ABI version " + std::to_string(mind->abi_version) + ", kernel speaks " + std::to_string(AIOS_ABI_VERSION)); }
  return std::unique_ptr<Intelligence>(new PluginIntelligence(h, mind, path));
}

}  // namespace aios
