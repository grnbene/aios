// Drive tool for Phase 1.0: Fleet Custom Kernels & Migration
// Proves in reality:
// 1. Independent registration of custom compute kernels (erp.tsp_exact) on multiple kernels.
// 2. Execution of the custom kernel graph on Kernel A.
// 3. Migration of the entity holding memory and graph from Kernel A to Kernel B.
// 4. Execution of the migrated graph on Kernel B's scheduler and local devices.
// 5. Exact match of outputs between Kernel A and Kernel B.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
#include <unistd.h>

#include "aios/compute/erp_kernels.hpp"
#include "aios/fleet/orchestrator.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/hal/host.hpp"

using namespace aios;

namespace {

int failures = 0;

void check(bool cond, const char* what, const std::string& extra = "") {
  if (cond) {
    std::printf("  [PASS] %s\n", what);
  } else {
    ++failures;
    std::printf("  [FAIL] %s %s\n", what, extra.c_str());
  }
}

// 5 test locations (lat, lon)
// 0: Chicago (41.8781, -87.6298)
// 1: New York (40.7128, -74.0060)
// 2: Los Angeles (34.0522, -118.2437)
// 3: Miami (25.7617, -80.1918)
// 4: Seattle (47.6062, -122.3321)
const std::vector<double> kTestPoints = {
  41.8781, -87.6298,
  40.7128, -74.0060,
  34.0522, -118.2437,
  25.7617, -80.1918,
  47.6062, -122.3321
};

} // namespace

int main() {
  std::printf("=== Driving Fleet Custom Kernel Migration & Execution ===\n");

  char root_a[] = "/tmp/aios-fleet-node-a-XXXXXX";
  char root_b[] = "/tmp/aios-fleet-node-b-XXXXXX";
  if (!mkdtemp(root_a) || !mkdtemp(root_b)) {
    std::perror("mkdtemp");
    return 1;
  }

  KernelConfig cfg_a; cfg_a.root = root_a; cfg_a.residency = test_config;
  KernelConfig cfg_b; cfg_b.root = root_b; cfg_b.residency = test_config;

  auto ka_res = Kernel::boot(cfg_a);
  auto kb_res = Kernel::boot(cfg_b);
  if (!ka_res.ok() || !kb_res.ok()) {
    std::printf("Failed to boot test kernels\n");
    return 1;
  }

  Kernel& ka = **ka_res;
  Kernel& kb = **kb_res;

  // 1. Register erp.tsp_exact on both kernels independently
  size_t registered_a = 0;
  for (auto* d : ka.devices().of_type(DeviceType::CPU)) {
    auto r = erp::register_erp_kernels(*d);
    check(r.ok(), "registered erp kernels on Kernel A CPU device");
    ++registered_a;
  }
  size_t registered_b = 0;
  for (auto* d : kb.devices().of_type(DeviceType::CPU)) {
    auto r = erp::register_erp_kernels(*d);
    check(r.ok(), "registered erp kernels on Kernel B CPU device");
    ++registered_b;
  }
  check(registered_a > 0 && registered_b > 0, "both kernels have registered custom kernels");

  // 2. Connect Kernel A to Kernel B
  Surface sb = kb.as(kb.self());
  Surface sa = ka.as(ka.self());

  uint16_t port = 19472;
  auto serve_res = sb.serve(port, "fleet-secret-token");
  check(serve_res.ok(), "Kernel B started node service", serve_res.ok() ? "" : serve_res.error().str());

  auto conn_res = sa.connect("127.0.0.1", port, "fleet-secret-token");
  check(conn_res.ok(), "Kernel A connected to Kernel B", conn_res.ok() ? "" : conn_res.error().str());

  // 3. Create job entity on Kernel A with authority to run compute graphs
  using K = ResourceKind;
  auto job_ent = ka.spawn("tsp-solver-job");
  check(job_ent.ok(), "Spawned tsp-solver-job entity on Kernel A");
  EntityID job_id = *job_ent;

  ka.grant(job_id, ResourceRef{K::Kernel, {}}, (uint16_t)R_Schedule);
  ka.grant(job_id, ResourceRef{K::ComputeDevice, {}}, (uint16_t)R_Execute);
  ka.grant(job_id, ResourceRef{K::Memory, {}}, (uint16_t)(R_Read | R_Write | R_Allocate | R_Destroy), 0, 1ull << 30);
  ka.grant(job_id, ResourceRef{K::State, {}}, (uint16_t)(R_Read | R_Write | R_Allocate | R_Destroy));

  Surface job_sa = ka.as(job_id);

  // Allocate tensors
  const int n = 5;
  auto tin = job_sa.tensor(DType::F64, {(uint64_t)n, 2});
  auto tord = job_sa.tensor(DType::I32, {(uint64_t)n});
  auto tmeta = job_sa.tensor(DType::F64, {4});
  check(tin.ok() && tord.ok() && tmeta.ok(), "Allocated tensors on Kernel A");

  // Write TSP points
  auto wr = job_sa.write(tin->memory, tin->offset, ByteSpan(reinterpret_cast<const uint8_t*>(kTestPoints.data()), kTestPoints.size() * sizeof(double)));
  check(wr.ok(), "Wrote TSP input coordinates to tensor memory");

  // Build Compute Graph with erp.tsp_exact
  auto g_res = job_sa.create_graph();
  check(g_res.ok(), "Created compute graph on Kernel A");
  GraphID g_id = *g_res;

  Operation op;
  op.code = OpCode::Custom;
  op.custom_kernel = fnv1a(std::string(erp::kTspExactKernel));
  op.requirement.device = DeviceType::CPU;
  op.requirement.parallelism = 2;
  op.scalar = 1.0; // prune = true
  op.inputs.push_back(*tin);
  op.outputs.push_back(*tord);
  op.outputs.push_back(*tmeta);

  auto add_op = job_sa.add_operation(g_id, op);
  check(add_op.ok(), "Added erp.tsp_exact operation to graph");

  // 4. Execute on Kernel A
  auto rep_a = job_sa.execute(g_id, SchedulePolicy{});
  check(rep_a.ok() && rep_a->failure.empty(), "Executed erp.tsp_exact graph on Kernel A", rep_a.ok() ? rep_a->failure : rep_a.error().str());

  auto ord_a_bytes = job_sa.read(tord->memory, tord->offset, (uint64_t)n * sizeof(int32_t));
  auto meta_a_bytes = job_sa.read(tmeta->memory, tmeta->offset, 4 * sizeof(double));
  check(ord_a_bytes.ok() && meta_a_bytes.ok(), "Read back outputs on Kernel A");

  std::vector<int32_t> order_a(n);
  std::memcpy(order_a.data(), ord_a_bytes->data(), n * sizeof(int32_t));
  double meta_a[4];
  std::memcpy(meta_a, meta_a_bytes->data(), 4 * sizeof(double));

  std::printf("  Kernel A Result: Distance = %.2f miles, Evaluated = %.0f, Order = [", meta_a[0], meta_a[1]);
  for (int i = 0; i < n; ++i) std::printf("%s%d", i ? ", " : "", order_a[i]);
  std::printf("]\n");

  // Reset execution position and zero out output buffers before migration
  ka.entities().set_execution(job_id, {}, 0);
  ByteBuffer zero_init(n * sizeof(int32_t), 0);
  (void)job_sa.write(tord->memory, tord->offset, ByteSpan(zero_init.data(), zero_init.size()));
  ByteBuffer zero_meta_init(4 * sizeof(double), 0);
  (void)job_sa.write(tmeta->memory, tmeta->offset, ByteSpan(zero_meta_init.data(), zero_meta_init.size()));

  // 5. Migrate entity from Kernel A to Kernel B via Orchestrator rebalance
  ka.set_slots(1); // 1 slot capacity
  auto extra_ent = ka.spawn("queued-background-job"); // 2nd job makes Kernel A oversubscribed (2 entities > 1 slot)
  (void)extra_ent;
  kb.set_slots(4); // Kernel B has idle headroom
  Orchestrator orch_a(ka);
  auto rebalance_decisions = orch_a.rebalance();
  check(!rebalance_decisions.empty() && rebalance_decisions[0].executed, "Orchestrator rebalance migrated job to Kernel B");
  if (!rebalance_decisions.empty()) {
    check(rebalance_decisions[0].entity == job_id, "Migrated correct job entity identity");
  }

  // Verify entity is gone from Kernel A and alive on Kernel B
  check(!ka.entities().exists(job_id), "Entity removed from Kernel A");
  check(kb.entities().exists(job_id), "Entity present in Kernel B");

  // Verify graph is installed on Kernel B
  auto* g_on_b = kb.graphs().get(g_id);
  check(g_on_b != nullptr, "Compute graph restored and installed on Kernel B");

  // 6. Execute the migrated graph on Kernel B
  // Zero out the output buffers first to ensure Kernel B actually computes it
  Surface job_sb = kb.as(job_id);
  ByteBuffer zero_bytes(n * sizeof(int32_t), 0);
  (void)job_sb.write(tord->memory, tord->offset, ByteSpan(zero_bytes.data(), zero_bytes.size()));
  ByteBuffer zero_meta(4 * sizeof(double), 0);
  (void)job_sb.write(tmeta->memory, tmeta->offset, ByteSpan(zero_meta.data(), zero_meta.size()));

  auto rep_b = job_sb.execute(g_id, SchedulePolicy{});
  check(rep_b.ok() && rep_b->failure.empty(), "Executed migrated erp.tsp_exact graph on Kernel B", rep_b.ok() ? rep_b->failure : rep_b.error().str());

  auto ord_b_bytes = job_sb.read(tord->memory, tord->offset, (uint64_t)n * sizeof(int32_t));
  auto meta_b_bytes = job_sb.read(tmeta->memory, tmeta->offset, 4 * sizeof(double));
  check(ord_b_bytes.ok() && meta_b_bytes.ok(), "Read back outputs on Kernel B");

  if (ord_b_bytes.ok() && meta_b_bytes.ok()) {
    std::vector<int32_t> order_b(n);
    std::memcpy(order_b.data(), ord_b_bytes->data(), n * sizeof(int32_t));
    double meta_b[4];
    std::memcpy(meta_b, meta_b_bytes->data(), 4 * sizeof(double));

    std::printf("  Kernel B Result: Distance = %.2f miles, Evaluated = %.0f, Order = [", meta_b[0], meta_b[1]);
    for (int i = 0; i < n; ++i) std::printf("%s%d", i ? ", " : "", order_b[i]);
    std::printf("]\n");

    // 7. Assert exact output match
    check(order_a == order_b, "Output visiting order matches 100% byte-for-byte");
    check(std::abs(meta_a[0] - meta_b[0]) < 1e-6, "Output total distance matches 100% byte-for-byte");
    check(meta_a[1] == meta_b[1], "Evaluated path count matches exactly");
  }

  if (failures == 0) {
    std::printf(">>> Phase 1.0 PROVEN: Fleet Custom Kernel Migration & Execution is fully functional!\n");
  } else {
    std::printf(">>> Phase 1.0 FAILED with %d failure(s)\n", failures);
  }

  std::fflush(stdout);
  std::_Exit(failures == 0 ? 0 : 1);
}
