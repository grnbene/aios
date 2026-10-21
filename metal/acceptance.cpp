// The bare-metal acceptance sequence: the unchanged kernel core, a
// deterministic mind, no Linux anywhere. Selected from the boot menu.
//   boot → hal::Host → Kernel::boot → info → entity → CPU device → matmul →
//   checkpoint → terminate → restore → verify state and tensor.
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include <cstring>
#include <cstdlib>

extern "C" void metal_console_write(const char* s, size_t n);
extern "C" size_t metal_heap_peak();
extern "C" size_t metal_heap_untouched();
extern "C" uint64_t metal_heap_coalesced();
extern "C" uint64_t metal_ap_jobs();
extern "C" int metal_ap_participants();

using namespace aios;

static void out(const char* s) { metal_console_write(s, strlen(s)); }
static void out(const std::string& s) { metal_console_write(s.data(), s.size()); }
static std::string num(uint64_t v) { return std::to_string(v); }
static std::string fnum(double v) { long long i = (long long)v; long long f = (long long)((v - i) * 1000 + (v >= 0 ? 0.5 : -0.5)); if (f < 0) f = -f; std::string s = std::to_string(i) + "."; if (f < 100) s += "0"; if (f < 10) s += "0"; return s + std::to_string(f); }


static int checks = 0, failures = 0;
static void check(bool ok, const std::string& what) { checks++; if (!ok) failures++; out(std::string(ok ? "  ok    " : "  FAIL  ") + what + "\n"); }

// Returns the number of failed checks.
int run_acceptance(Kernel& k) {
  checks = failures = 0;
  // ---- the kernel, unchanged -------------------------------------------------
  auto& topo = k.host().topology();
  out("kernel   " + k.self().str() + "\n");
  out("host     " + topo.arch + " · " + topo.host_name + " · " + num(topo.logical_cpus) + " cpu · " + num(topo.total_memory >> 20) + " MiB\n");
  out("devices  ");
  for (auto* d : k.devices().all()) out(d->name() + "(" + d->id().short_str() + ", " + num(d->capacity().lanes) + " lanes) ");
  out("\naccel    " + (k.accelerators().devices.empty() ? k.accelerators().note : num(k.accelerators().devices.size())) + "\n\n");

  out("acceptance\n");
  check(k.devices().count() >= 1, "a CPU device exists in the fabric");
  auto e = k.spawn("first-entity");
  check(e.ok(), "entity spawned");
  k.grant(*e, ResourceRef{ResourceKind::Kernel, {}}, R_Schedule);
  k.grant(*e, ResourceRef{ResourceKind::ComputeDevice, {}}, R_Execute);
  k.grant(*e, ResourceRef{ResourceKind::Kernel, {}}, R_Checkpoint);
  Surface s = k.as(*e);
  auto A = s.tensor(DType::F32, {2, 3}); auto B = s.tensor(DType::F32, {3, 2}); auto C = s.tensor(DType::F32, {2, 2}); auto S = s.tensor(DType::F32, {1});
  check(A.ok() && B.ok() && C.ok() && S.ok(), "tensors allocated through the memory fabric");
  float a[6] = {1, 2, 3, 4, 5, 6}, b[6] = {7, 8, 9, 10, 11, 12};
  s.write(A->memory, 0, ByteSpan((const uint8_t*)a, 24)); s.write(B->memory, 0, ByteSpan((const uint8_t*)b, 24));
  auto g = s.create_graph();
  Operation mm; mm.code = OpCode::MatMul; mm.inputs = {*A, *B}; mm.outputs = {*C}; mm.requirement.parallelism = 2;
  Operation rs; rs.code = OpCode::ReduceSum; rs.inputs = {*C}; rs.outputs = {*S};
  s.add_operation(*g, mm); s.add_operation(*g, rs);
  auto rep = s.execute(*g);
  check(rep.ok() && rep->complete && rep->failure.empty(), "graph scheduled and executed on the CPU device (" + (rep.ok() ? num(rep->operations_run) + " ops, " + num(rep->elapsed_ns / 1000) + " µs" : rep.error().str()) + ")");
  auto cbytes = s.read(C->memory, 0, 16); auto sbytes = s.read(S->memory, 0, 4);
  float c[4] = {0, 0, 0, 0}, sum = 0;
  if (cbytes.ok()) memcpy(c, cbytes->data(), 16);
  if (sbytes.ok()) memcpy(&sum, sbytes->data(), 4);
  out("  matmul = [" + fnum(c[0]) + " " + fnum(c[1]) + " " + fnum(c[2]) + " " + fnum(c[3]) + "]  sum = " + fnum(sum) + "\n");
  check(c[0] == 58 && c[1] == 64 && c[2] == 139 && c[3] == 154 && sum == 415, "matmul [58 64 139 154], reduce 415");
  // SMP: a larger matmul spread across cores. A(64x64)=1, B(64x64)=1 -> C=64.
  if (topo.logical_cpus > 1) {
    // A separate worker entity, so this does not disturb the entity whose
    // execution position the checkpoint checks below verify.
    auto we = k.spawn("smp-worker");
    k.grant(*we, ResourceRef{ResourceKind::Kernel, {}}, R_Schedule);
    k.grant(*we, ResourceRef{ResourceKind::ComputeDevice, {}}, R_Execute);
    Surface ws = k.as(*we);
    uint64_t jobs0 = metal_ap_jobs();
    auto MA = ws.tensor(DType::F32, {64, 64}); auto MB = ws.tensor(DType::F32, {64, 64}); auto MC = ws.tensor(DType::F32, {64, 64});
    if (MA.ok() && MB.ok() && MC.ok()) {
      std::vector<float> ones(64 * 64, 1.0f);
      ws.write(MA->memory, 0, ByteSpan((const uint8_t*)ones.data(), ones.size() * 4));
      ws.write(MB->memory, 0, ByteSpan((const uint8_t*)ones.data(), ones.size() * 4));
      auto pg = ws.create_graph();
      Operation pm; pm.code = OpCode::MatMul; pm.inputs = {*MA, *MB}; pm.outputs = {*MC}; pm.requirement.parallelism = topo.logical_cpus;
      ws.add_operation(*pg, pm);
      auto prep = ws.execute(*pg);
      auto cb = ws.read(MC->memory, 0, 4); float c0 = 0; if (cb.ok()) memcpy(&c0, cb->data(), 4);
      uint64_t ran = metal_ap_jobs() - jobs0;
      check(prep.ok() && prep->complete && c0 == 64.0f, "parallel matmul on " + num(topo.logical_cpus) + " cores correct (C[0]=" + fnum(c0) + ")");
      check(ran > 0, "work ran on application processors (" + num(ran) + " AP jobs, " + num((uint64_t)metal_ap_participants()) + " cores)");
    }
    k.terminate(*we);
  }
  auto st = s.create_state(StateKind::Persistent, span_of(std::string("I am the first entity on bare metal")));
  s.attach(*st, C->memory);
  // a state channel round-trip
  auto ch = s.create_channel(4, 256); auto seq = s.publish(*ch, *st); auto fr = s.consume(*ch);
  check(ch.ok() && seq.ok() && fr.ok() && string_of(fr->payload) == "I am the first entity on bare metal", "state published into a channel and consumed");
  // a deterministic mind, one cycle
  auto ctx = s.create_state(StateKind::Context, span_of(std::string("hello, body")));
  k.bind_mind(*e, make_echo_intelligence());
  auto cyc = k.cycle(*e);
  auto res = cyc.ok() ? k.states().copy(cyc->result) : Result<StateObject>(Status::NotFound);
  check(cyc.ok() && res.ok() && string_of(res->payload).find("hello, body") != std::string::npos, "cognitive cycle ran (mind: echo)");
  // checkpoint → terminate → restore → verify
  auto image = k.capture(*e);
  check(image.ok(), "entity captured (" + (image.ok() ? num(image->size()) + " bytes" : "") + ")");
  auto path = k.checkpoint(*e);
  check(path.ok(), "checkpoint written to the persistent root " + (path.ok() ? *path : path.error().str()));
  EntityID id = *e;
  k.terminate(id);
  check(!k.entities().exists(id), "entity terminated");
  auto restored = k.restore(*path);
  check(restored.ok() && *restored == id, "entity restored with the SAME identity");
  auto st2 = k.states().copy(*st);
  check(st2.ok() && string_of(st2->payload) == "I am the first entity on bare metal", "persistent state intact after restore");
  auto c2 = k.memory().read(C->memory, 0, 16);
  float cc[4] = {0, 0, 0, 0}; if (c2.ok()) memcpy(cc, c2->data(), 16);
  check(cc[0] == 58 && cc[3] == 154, "tensor memory intact after restore (same MemoryID)");
  auto caps = k.capabilities().list(id);
  check(caps.size() >= 5, "capabilities restored (" + num(caps.size()) + ")");
  auto ent = k.entities().copy(id);
  check(ent.ok() && !ent->execution.is_null() && ent->execution_position == 2, "execution position restored (" + (ent.ok() ? num(ent->execution_position) : "") + ")");

  // ---- authority: a resident console holds kernel ACCESS, never kernel OWNERSHIP ----
  // The console is an entity with a recorded, bounded capability set. Whatever
  // sits at it (a human, a mind) can only ask; the kernel answers from that set.
  AuthorityProfile prof = {
    {ResourceRef{ResourceKind::Kernel, {}}, (uint16_t)(R_Read | R_Execute | R_Checkpoint | R_Schedule | R_Grant), 0},
    {ResourceRef{ResourceKind::Entity, {}}, (uint16_t)(R_Allocate | R_Execute | R_Destroy | R_Grant), 0},
    {ResourceRef{ResourceKind::ComputeDevice, {}}, (uint16_t)(R_Execute | R_Grant), 0}};
  auto con = k.resident("acceptance-console", prof);
  check(con.ok() && k.is_resident(*con), "a resident console entity exists with recorded authority");
  Surface cs = k.as(*con);
  auto self_grant = cs.grant(*con, ResourceRef{ResourceKind::Kernel, {}}, R_All);
  check(!self_grant.ok() && self_grant.status() == Status::Denied, "console asks for kernel:* all → " + (self_grant.ok() ? std::string("GRANTED?!") : self_grant.error().message));
  auto req = cs.request(ResourceRef{ResourceKind::Kernel, {}}, R_Write);
  check(!req.ok() && req.status() == Status::Denied, "console requests kernel:write → refused by the kernel");
  auto ok_req = cs.request(ResourceRef{ResourceKind::Kernel, {}}, R_Schedule);
  check(ok_req.ok(), "console requests kernel:schedule → permitted (it holds it)");
  auto as_kernel = cs.act_as(k.self());
  check(!as_kernel.ok(), "no entity acts as the kernel entity");
  // Authority does not travel in images: kernel:* all planted in a checkpoint by
  // kernel authority does not come back when the CONSOLE restores it.
  auto smug = cs.spawn("smuggler");
  k.grant(*smug, ResourceRef{ResourceKind::Kernel, {}}, R_All);
  auto simg = k.checkpoint(*smug);
  k.terminate(*smug);
  auto back = simg.ok() ? cs.restore(*simg) : Result<EntityID>(Status::IoError);
  bool owns_kernel = false;
  if (back.ok()) for (auto& c : k.capabilities().list(*back)) if (c.resource.kind == ResourceKind::Kernel && (c.rights & (R_Write | R_Destroy | R_Allocate))) owns_kernel = true;
  check(back.ok() && !owns_kernel, "restore by the console re-derives authority: kernel:* all did not come back");
  // ---- governed self-improvement: the machine decides, not the entity ----
  // A trusted resident may petition to grow its authority; the machine grants,
  // bounded, never kernel ownership. A plain entity may not petition at all.
  { // a bare trusted resident (self-improver, holds nothing over the kernel yet)
    auto petitioner = k.resident("petitioner", {}, /*self_improve=*/true);
    Surface rs = k.as(*petitioner);
    bool held_before = k.capabilities().check(*petitioner, ResourceRef{ResourceKind::Kernel, {}}, R_Read, k.now());
    auto p_access = rs.request(ResourceRef{ResourceKind::Kernel, {}}, R_Read | R_Execute | R_Schedule);
    check(!held_before && p_access.ok() && k.capabilities().check(*petitioner, ResourceRef{ResourceKind::Kernel, {}}, R_Schedule, k.now()),
          "a trusted resident that held no kernel authority petitions → the machine MINTS bounded kernel access");
    auto p_own = rs.request(ResourceRef{ResourceKind::Kernel, {}}, R_Write | R_Destroy);
    check(!p_own.ok() && p_own.status() == Status::Denied && !k.capabilities().check(*petitioner, ResourceRef{ResourceKind::Kernel, {}}, R_Write, k.now()),
          "the same resident petitions for kernel ownership → refused (the hard wall), and it did NOT get it");
    auto p_mem = rs.request(ResourceRef{ResourceKind::Memory, {}}, R_Allocate);
    const Capability* mc = p_mem.ok() ? k.capabilities().get(*p_mem) : nullptr;
    check(mc && mc->scope_limit > 0, "a petition for memory is granted BOUNDED (a ceiling, never unbounded)");
    auto plain = k.spawn("outsider");
    Surface ps = k.as(*plain);
    auto p_plain = ps.request(ResourceRef{ResourceKind::ComputeDevice, {}}, R_Execute);
    check(!p_plain.ok() && p_plain.status() == Status::Denied && !k.may_self_improve(*plain),
          "a plain entity petitions → refused: self-improvement is reserved to trusted residents");
    k.terminate(*plain);
  }
  int refusals = 0; for (auto& ev : k.authority().events()) if (ev.kind == AuthorityEvent::Refused) refusals++;
  check(refusals >= 2, "the refusals are on the kernel's persistent record (" + num(refusals) + ")");

  // ---- the heap: freed blocks of one size must be able to serve a request of another ----------
  {
    const uint64_t before = metal_heap_coalesced();
    void* head = nullptr;
    while (metal_heap_untouched() > (512u << 10)) { void** b = (void**)malloc(4080); *b = head; head = b; }   // spend the bump region on 4 KiB blocks
    while (head) { void* next = *(void**)head; free(head); head = next; }
    void* big = malloc(1u << 20);   // no freed block is of this class, and nothing uncut is left: only joining neighbours can serve it
    check(big != nullptr && metal_heap_coalesced() > before, "a heap spent on small blocks, all freed, still serves a 1 MiB request (neighbours joined)");
    free(big);
  }

  out("\n" + num(checks - failures) + "/" + num(checks) + " checks passed · heap peak " + num(metal_heap_peak() >> 10) + " KiB · uptime " + num(k.uptime_ns() / 1000000) + " ms\n");
  out(failures == 0 ? "ACCEPTANCE: PASS — the kernel ran with no Linux underneath\n" : "ACCEPTANCE: FAIL\n");
  return failures;
}
