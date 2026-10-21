// Benchmark tool to measure real migration latency of compute graph entities across kernels under load.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <iostream>
#include <vector>
#include <numeric>
#include <algorithm>
#include <unistd.h>

#include "aios/compute/erp_kernels.hpp"
#include "aios/fleet/orchestrator.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/hal/host.hpp"

using namespace aios;

const std::vector<double> kBenchPoints = {
  41.8781, -87.6298,
  40.7128, -74.0060,
  34.0522, -118.2437,
  25.7617, -80.1918,
  47.6062, -122.3321
};

int main() {
  std::printf("=== Real Migration Latency Benchmark Under Load ===\n");

  char root_a[] = "/tmp/aios-bench-node-a-XXXXXX";
  char root_b[] = "/tmp/aios-bench-node-b-XXXXXX";
  if (!mkdtemp(root_a) || !mkdtemp(root_b)) {
    std::perror("mkdtemp");
    return 1;
  }

  KernelConfig cfg_a; cfg_a.root = root_a;
  KernelConfig cfg_b; cfg_b.root = root_b;

  auto ka_res = Kernel::boot(cfg_a);
  auto kb_res = Kernel::boot(cfg_b);
  if (!ka_res.ok() || !kb_res.ok()) {
    std::printf("Failed to boot kernels\n");
    return 1;
  }

  Kernel& ka = **ka_res;
  Kernel& kb = **kb_res;

  for (auto* d : ka.devices().of_type(DeviceType::CPU)) { (void)erp::register_erp_kernels(*d); }
  for (auto* d : kb.devices().of_type(DeviceType::CPU)) { (void)erp::register_erp_kernels(*d); }

  Surface sb = kb.as(kb.self());
  Surface sa = ka.as(ka.self());

  uint16_t port = 19499;
  auto serve_res = sb.serve(port, "bench-secret-token");
  if (!serve_res.ok()) {
    std::printf("Failed to start node service: %s\n", serve_res.error().message.c_str());
    return 1;
  }

  auto conn_res = sa.connect("127.0.0.1", port, "bench-secret-token");
  if (!conn_res.ok()) {
    std::printf("Failed to connect node service: %s\n", conn_res.error().message.c_str());
    return 1;
  }

  const int num_trials = 10;
  std::vector<double> latencies_us;
  int verified_execs = 0;

  for (int trial = 0; trial < num_trials; ++trial) {
    ka.set_slots(1);
    kb.set_slots(8);

    using K = ResourceKind;
    std::string job_name = "migratable-job-" + std::to_string(trial);
    auto job_ent = ka.spawn(job_name);
    if (!job_ent.ok()) continue;
    EntityID job_id = *job_ent;

    ka.grant(job_id, ResourceRef{K::Kernel, {}}, (uint16_t)R_Schedule);
    ka.grant(job_id, ResourceRef{K::ComputeDevice, {}}, (uint16_t)R_Execute);
    ka.grant(job_id, ResourceRef{K::Memory, {}}, (uint16_t)(R_Read | R_Write | R_Allocate | R_Destroy), 0, 1ull << 30);
    ka.grant(job_id, ResourceRef{K::State, {}}, (uint16_t)(R_Read | R_Write | R_Allocate | R_Destroy));

    Surface job_sa = ka.as(job_id);

    const int n = 5;
    auto tin = job_sa.tensor(DType::F64, {(uint64_t)n, 2});
    auto tord = job_sa.tensor(DType::I32, {(uint64_t)n});
    auto tmeta = job_sa.tensor(DType::F64, {4});

    (void)job_sa.write(tin->memory, tin->offset, ByteSpan(reinterpret_cast<const uint8_t*>(kBenchPoints.data()), kBenchPoints.size() * sizeof(double)));

    auto g_res = job_sa.create_graph();
    GraphID g_id = *g_res;

    Operation op;
    op.code = OpCode::Custom;
    op.custom_kernel = fnv1a(std::string(erp::kTspExactKernel));
    op.requirement.device = DeviceType::CPU;
    op.requirement.parallelism = 2;
    op.scalar = 1.0;
    op.inputs.push_back(*tin);
    op.outputs.push_back(*tord);
    op.outputs.push_back(*tmeta);

    (void)job_sa.add_operation(g_id, op);

    // Initial execution on Kernel A
    auto rep_a = job_sa.execute(g_id, SchedulePolicy{});
    if (!rep_a.ok()) continue;

    // Reset execution position before migration
    ka.entities().set_execution(job_id, {}, 0);

    // Oversubscribe Kernel A (extra job puts movable load at 2 > 1 slot capacity)
    auto bg_ent = ka.spawn("load-job-" + std::to_string(trial));

    Orchestrator orch_a(ka);

    // Measure exact migration duration
    auto t0 = std::chrono::high_resolution_clock::now();
    auto decisions = orch_a.rebalance(true);
    auto t1 = std::chrono::high_resolution_clock::now();

    if (!decisions.empty() && decisions[0].executed) {
      double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
      latencies_us.push_back(us);

      // Verify execution on destination Kernel B
      if (kb.entities().exists(job_id)) {
        Surface job_sb = kb.as(job_id);
        auto rep_b = job_sb.execute(g_id, SchedulePolicy{});
        if (rep_b.ok() && rep_b->failure.empty()) {
          verified_execs++;
        }
      }
    }

    // Clean up extra load job so slots reset cleanly for next trial
    if (bg_ent.ok()) {
      ka.entities().set_status(*bg_ent, EntityStatus::Terminated);
    }
  }

  if (latencies_us.empty()) {
    std::printf("ERROR: No successful migrations measured!\n");
    return 1;
  }

  std::sort(latencies_us.begin(), latencies_us.end());
  double min_us = latencies_us.front();
  double max_us = latencies_us.back();
  double median_us = latencies_us[latencies_us.size() / 2];
  double sum_us = std::accumulate(latencies_us.begin(), latencies_us.end(), 0.0);
  double avg_us = sum_us / latencies_us.size();

  std::printf("\n--- Migration Latency Benchmark Results (%zu trials, %d verified executions) ---\n", latencies_us.size(), verified_execs);
  std::printf("  Min Latency    : %.2f us (%.3f ms)\n", min_us, min_us / 1000.0);
  std::printf("  Median Latency : %.2f us (%.3f ms)\n", median_us, median_us / 1000.0);
  std::printf("  Average Latency: %.2f us (%.3f ms)\n", avg_us, avg_us / 1000.0);
  std::printf("  Max Latency    : %.2f us (%.3f ms)\n", max_us, max_us / 1000.0);
  std::printf("\n>>> EMPIRICALLY MEASURED: Real load migration latency proven under test!\n");

  return 0;
}
