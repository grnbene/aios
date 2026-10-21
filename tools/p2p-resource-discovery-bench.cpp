// Builds a multi-node mesh and checks Kademlia XOR-metric routing, mDNS peer-table formation,
// resource announcements into a semantic catalog, and resolving a resource to its holder.

#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "aios/channel/peer_discovery.hpp"

using namespace aios;
using namespace aios::channel;

static int failures = 0;

static void check(bool condition, const std::string& name) {
  if (condition) {
    std::cout << "  [PASS] " << name << "\n";
  } else {
    std::cout << "  [FAIL] " << name << "\n";
    failures++;
  }
}

int main() {
  std::cout << "======================================================================\n";
  std::cout << "    AIOS P2P RESOURCE DISCOVERY BENCHMARK & MULTI-NODE PROOF         \n";
  std::cout << "      'MAKING MACHINE LOCATION VANISH' - REAL KADEMLIA & DHT         \n";
  std::cout << "======================================================================\n\n";

  // 1. Multi-Node Mesh Initialization (Simulating the Real Fleet)
  std::cout << "─── [1] Multi-Node Mesh Initialization & Kademlia XOR Metric ─────────\n";
  
  P2PMeshDiscovery node_arch("arch-workstation-seed");
  P2PMeshDiscovery node_chi("chi-il-1-seed");
  P2PMeshDiscovery node_devops("devops-runner-seed");
  P2PMeshDiscovery node_rotors("rotors-gpu-seed");
  P2PMeshDiscovery node_skyblade("skyblade-edge-seed");
  P2PMeshDiscovery node_windows("windows-gui-seed");

  std::cout << "  Arch Node ID     : " << node_arch.local_node_id() << "\n";
  std::cout << "  Chi Node ID      : " << node_chi.local_node_id() << "\n";
  std::cout << "  Rotors Node ID   : " << node_rotors.local_node_id() << "\n";

  NodeID d_arch_chi = xor_distance(hex_to_node_id(node_arch.local_node_id()), hex_to_node_id(node_chi.local_node_id()));
  int cpl_arch_chi = common_prefix_length(d_arch_chi);
  check(node_id_to_hex(d_arch_chi).length() == 40, "Kademlia 160-bit XOR distance computed");
  check(cpl_arch_chi >= 0 && cpl_arch_chi <= 160, "Common prefix length in valid range [0, 160]");

  // 2. mDNS Beacon & Peer Discovery
  std::cout << "\n─── [2] mDNS Beacons & Autonomous Peer Table Formation ───────────────\n";
  std::string beacon_chi = node_chi.generate_mdns_announcement(7420);
  auto parsed_chi = node_arch.parse_mdns_announcement(beacon_chi);
  check(parsed_chi.ok(), "Parse Chi mDNS beacon announcement");
  check(parsed_chi->node_id == node_chi.local_node_id(), "Chi node ID correctly extracted from beacon");

  // Form the mesh by introducing peers to Arch
  node_arch.ping_peer(node_chi.local_node_id(), "203.0.113.10", 7420);
  node_arch.ping_peer(node_devops.local_node_id(), "192.168.1.102", 7420);
  node_arch.ping_peer(node_rotors.local_node_id(), "192.168.1.105", 7420);
  node_arch.ping_peer(node_skyblade.local_node_id(), "192.168.1.110", 7420);
  node_arch.ping_peer(node_windows.local_node_id(), "192.168.1.120", 7420);

  check(node_arch.active_peer_count() == 5, "Arch discovered and holds 5 active peer nodes");
  check(node_arch.active_bucket_count() >= 1, "K-buckets correctly populated with non-zero routing depth");

  // 3. Resource Announcements (Objects, Models, Compute, Entities)
  std::cout << "\n─── [3] Multi-Node Resource Announcements & Semantic Catalog ─────────\n";
  
  // Chi announces ERP database & in-memory ASQL compute
  node_chi.announce_resource("Compute/ASQL-ERP", "engine=asql;tables=50;cdc=mysql", 8 /* 8 lanes */, "203.0.113.10", 7311);
  node_chi.announce_resource("Object/salesorders-902628", "type=salesorder;debtor=CUST_42;amount=1250.00", 902628, "203.0.113.10", 3307);

  // Rotors announces high-end GPU compute
  node_rotors.announce_resource("Compute/CUDA-RTX4090", "arch=ada;vram=24GB;lanes=16384", 24ULL * 1024 * 1024 * 1024, "192.168.1.105", 7312);
  node_rotors.announce_resource("Model/gemma-2b", "format=gguf;ctx=8192;quant=q4_k_m", 2ULL * 1024 * 1024 * 1024, "192.168.1.105", 7312);

  // Arch announces local state & entity
  node_arch.announce_resource("Entity/902628", "status=processing;assigned=arch", 848 /* generation */, "127.0.0.1", 7310);

  // Sync resource ads into Arch's DHT view
  auto chi_resources = node_chi.list_resources_by_prefix("");
  for (const auto& r : chi_resources) {
    node_arch.store_resource(r);
  }
  auto rotors_resources = node_rotors.list_resources_by_prefix("");
  for (const auto& r : rotors_resources) {
    node_arch.store_resource(r);
  }

  check(node_arch.resource_count() == 5, "Arch catalog tracks 5 distributed resources across fleet");

  // 4. Resource Queries: 'find Object/*', 'find Model/*', 'find Compute/*'
  std::cout << "\n─── [4] Autonomous Resource Resolution & Target Routing ──────────────\n";

  auto t0 = std::chrono::high_resolution_clock::now();
  auto res_model = node_arch.find_resource("Model/gemma-2b");
  auto t1 = std::chrono::high_resolution_clock::now();
  double us_model = std::chrono::duration<double, std::micro>(t1 - t0).count();

  check(res_model.found, "find Model/gemma-2b resolved");
  check(res_model.resource.provider_node_id == node_rotors.local_node_id(), "Model/gemma-2b routed directly to Rotors node");
  check(res_model.resource.provider_ip == "192.168.1.105", "Model/gemma-2b provider IP matches Rotors (192.168.1.105)");
  std::cout << "  Model lookup latency : " << std::fixed << std::setprecision(2) << us_model << " µs\n";

  auto res_asql = node_arch.find_resource("Compute/ASQL-ERP");
  check(res_asql.found, "find Compute/ASQL-ERP resolved");
  check(res_asql.resource.provider_node_id == node_chi.local_node_id(), "Compute/ASQL-ERP routed directly to Chi node");
  check(res_asql.resource.provider_port == 7311, "Compute/ASQL-ERP port matches 7311");

  auto res_order = node_arch.find_resource("Object/salesorders-902628");
  check(res_order.found, "find Object/salesorders-902628 resolved");
  check(res_order.resource.capacity == 902628, "Order entity capacity/ID verified");

  // 5. Prefix Discovery (e.g. all Compute resources on fleet)
  std::cout << "\n─── [5] Categorical Resource Prefix Discovery ────────────────────────\n";
  auto all_compute = node_arch.list_resources_by_prefix("Compute/");
  check(all_compute.size() == 2, "Discovered all Compute resources across fleet (2 found)");
  for (const auto& c : all_compute) {
    std::cout << "  Discovered Compute: " << c.resource_uri << " @ " << c.provider_ip << ":" << c.provider_port
              << " (" << c.metadata << ")\n";
  }

  // 6. Closest-Peer Routing on Cache Miss / External Resource
  std::cout << "\n─── [6] Kademlia Closest-Peer Routing for Unknown Keys ───────────────\n";
  auto missing_res = node_arch.find_resource("Model/deepseek-v3");
  check(!missing_res.found, "Unknown resource correctly triggers Kademlia peer routing");
  check(!missing_res.closest_peers.empty(), "Kademlia closest peers returned for routing iteration");
  std::cout << "  Closest peers returned for iterative lookup: " << missing_res.closest_peers.size() << " nodes\n";

  std::cout << "\n======================================================================\n";
  std::cout << "  SUMMARY: " << (failures == 0 ? "ALL P2P RESOURCE DISCOVERY CHECKS PASSED ✅" : "FAILURES DETECTED ❌") << "\n";
  std::cout << "======================================================================\n";

  return failures == 0 ? 0 : 1;
}
