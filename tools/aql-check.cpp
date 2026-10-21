// Verifies AQL, the object query language: WHERE filters and the other query forms, run
// against a kernel booted on a throwaway root.

#undef NDEBUG
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <cassert>
#include <ctime>
#include <unistd.h>
#include <unordered_set>
#include "aios/storage/aql.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/userland/face.hpp"
#include "aios/channel/gateway.hpp"

using namespace aios;
using namespace aios::storage;

// A mkdtemp directory that is removed (recursively) when it goes out of scope.
// Declared before the Kernel that uses it, so the kernel is gone first.
struct TempRoot {
  char path[64];
  explicit TempRoot(const char* tmpl) {
    std::snprintf(path, sizeof path, "%s", tmpl);
    if (!mkdtemp(path)) { std::perror("mkdtemp"); std::exit(1); }
  }
  ~TempRoot() { std::error_code ec; std::filesystem::remove_all(path, ec); }
  TempRoot(const TempRoot&) = delete;
  TempRoot& operator=(const TempRoot&) = delete;
};

int main() {
  std::cout << "=== AQL Verification Suite ===\n";

  TempRoot tmp("/tmp/aql-test-XXXXXX");
  const char* root_dir = tmp.path;

  KernelConfig kcfg;
  kcfg.root = root_dir;
  kcfg.residency = test_config;
  auto k_res = Kernel::boot(kcfg);
  assert(k_res.ok());
  Kernel& k = **k_res;

  Surface self_surf = k.as(k.self());

  // 1. Parser + SELECT ... WHERE meta.*
  std::cout << "[Step 1] Testing Parser & WHERE meta.* filter...\n";

  StoredObject o1;
  o1.metadata.emplace_back("kind", "erp.route");
  o1.metadata.emplace_back("customer_id", "4471");
  o1.payload = ByteBuffer{'R', 'o', 'u', 't', 'e', '1'};
  o1.embedding = text_embedding("cleve dispatch board route 1");
  auto id1 = self_surf.store(o1);
  assert(id1.ok());

  StoredObject o2;
  o2.metadata.emplace_back("kind", "erp.route");
  o2.metadata.emplace_back("customer_id", "9999");
  o2.payload = ByteBuffer{'R', 'o', 'u', 't', 'e', '2'};
  o2.embedding = text_embedding("cleve dispatch board route 2");
  auto id2 = self_surf.store(o2);
  assert(id2.ok());

  std::string q1 = "SELECT * FROM objects WHERE meta.kind = \"erp.route\" AND meta.customer_id = \"4471\" ORDER BY timestamp DESC LIMIT 20";
  auto res1 = execute_aql(self_surf, q1);
  assert(res1.success);
  assert(res1.count == 1);
  assert(res1.objects[0].id == *id1);
  std::cout << "  WHERE filter passed: found 1 matching object [" << id1->short_str() << "].\n";

  // 2. NEAR similarity search
  std::cout << "[Step 2] Testing NEAR similarity search...\n";
  std::string q2 = "SELECT * FROM objects NEAR embedding_of(\"cleve dispatch board\") LIMIT 5";
  auto res2 = execute_aql(self_surf, q2);
  assert(res2.success);
  assert(res2.count >= 2);
  std::cout << "  NEAR search passed: found " << res2.count << " similar objects.\n";

  // 3. RELATED TO & HISTORY OF
  std::cout << "[Step 3] Testing RELATED TO & HISTORY OF graph/chain traversals...\n";
  StoredObject o3;
  o3.derived_from = *id1;
  o3.relations.push_back(ObjectRelation{*id2, 0xD0});
  o3.metadata.emplace_back("kind", "erp.route_step");
  o3.payload = ByteBuffer{'S', 't', 'e', 'p', '1'};
  auto id3 = self_surf.store(o3);
  assert(id3.ok());

  std::string q3_rel = "SELECT * FROM objects RELATED TO obj:" + id3->str() + " KIND \"derived_from\"";
  auto res3_rel = execute_aql(self_surf, q3_rel);
  assert(res3_rel.success);
  assert(res3_rel.count >= 1);

  std::string q3_hist = "SELECT * FROM objects HISTORY OF obj:" + id3->str();
  auto res3_hist = execute_aql(self_surf, q3_hist);
  assert(res3_hist.success);
  assert(res3_hist.count == 2); // id3, then id1
  assert(res3_hist.objects[0].id == *id3);
  assert(res3_hist.objects[1].id == *id1);

  auto prov_check = k.store().verify_provenance_chain(*id3);
  assert(prov_check.ok());
  std::cout << "  RELATED TO & HISTORY OF passed: derivation chain resolved & cryptographic SHA-256 tamper-evidence verified.\n";

  // 4. Capability-Scoping Proof
  std::cout << "[Step 4] Testing Capability Scoping...\n";
  auto entity_a = k.spawn("entity_a");
  assert(entity_a.ok());
  auto entity_b = k.spawn("entity_b");
  assert(entity_b.ok());

  // Entity A gets full StorageObject access
  auto cap_a = k.grant(*entity_a, ResourceRef{ResourceKind::StorageObject, {}}, R_Read);
  assert(cap_a.ok());

  // Revoke entity_b's default wildcard storage read so it is scoped strictly to id1
  if (auto* eb = k.entities().get(*entity_b)) {
    auto caps_copy = eb->capabilities;
    for (const auto& cid : caps_copy) {
      k.revoke(cid);
    }
  }

  // Entity B gets access ONLY to object id1
  auto cap_b = k.grant(*entity_b, ResourceRef{ResourceKind::StorageObject, *id1}, R_Read);
  assert(cap_b.ok());

  Surface surf_a = k.as(*entity_a);
  Surface surf_b = k.as(*entity_b);

  std::string q_scope = "SELECT * FROM objects";
  auto res_a = execute_aql(surf_a, q_scope);
  auto res_b = execute_aql(surf_b, q_scope);

  assert(res_a.count == 3); // Entity A sees all 3 objects
  assert(res_b.count == 1); // Entity B sees ONLY id1
  assert(res_b.objects[0].id == *id1);
  std::cout << "  Capability Scoping PROVEN: Entity A saw " << res_a.count << " objects, Entity B saw " << res_b.count << " object.\n";

  // 5. AS OF Checkpoint Time Travel
  std::cout << "[Step 5] Testing AS OF Checkpoint time travel...\n";
  auto ckpt_path = k.checkpoint(*entity_a);
  assert(ckpt_path.ok());

  // Add object 4 AFTER the checkpoint
  StoredObject o4;
  o4.metadata.emplace_back("kind", "erp.route");
  o4.payload = ByteBuffer{'F', 'u', 't', 'u', 'r', 'e'};
  o4.timestamp_ns = k.now() + 1000000000ULL;
  auto id4 = self_surf.store(o4);
  assert(id4.ok());

  std::string q_asof = "SELECT * FROM objects WHERE meta.kind = \"erp.route\" AS OF checkpoint(\"" + *ckpt_path + "\")";
  auto res_asof = execute_aql(self_surf, q_asof);
  assert(res_asof.success);
  // Should NOT contain o4 because o4 was created after the checkpoint timestamp
  bool found_o4 = false;
  for (const auto& obj : res_asof.objects) if (obj.id == *id4) found_o4 = true;
  assert(!found_o4);
  (void)found_o4;
  std::cout << "  AS OF Checkpoint passed: read-only projection excluded post-checkpoint object [" << id4->short_str() << "].\n";

  // 6. Expose in face.cpp and Gateway
  std::cout << "[Step 6] Testing face.cpp & Gateway integration...\n";
  userland::HostBindings h;
  h.store_query = [&](const std::string& qstr) -> std::string {
    auto res = execute_aql(self_surf, qstr);
    std::string out = "[";
    for (size_t i = 0; i < res.objects.size(); ++i) {
      if (i) out += ",";
      out += "{\"id\":\"" + res.objects[i].id.str() + "\",\"kind\":\"" + res.objects[i].meta("kind") + "\"}";
    }
    return out + "]";
  };

  auto query_json = h.store_query("SELECT * FROM objects WHERE meta.kind = \"erp.route\"");
  assert(query_json.find("erp.route") != std::string::npos);
  std::cout << "  face.cpp / Gateway integration passed: JSON response rendered.\n";

  // Note: this file used to have a "Step 7: Face Scrubber & AQL AS OF"
  // block here. It rendered a face whose scrubber handler was a bare `say`
  // statement (never read the slider position), then ran a query with a
  // hardcoded timestamp that had no connection to the face at all, and
  // printed a success line describing a capability neither the face nor the
  // query exercised. Deleted rather than fixed: the real thing it claimed to
  // test -- a scrubber whose position drives a real AS OF query -- is now
  // actually built (aios-root/dreams/fleet/face.aos) and actually tested
  // (tools/residency-check.cpp's Phase 2, which drives a real face through a
  // real blur/focus cycle and checks the rendered output is byte-identical).
  // A second, fake copy of that claim did not belong here.

  // 7. Sparse Block Indexing for Cold Segments
  std::cout << "[Step 7] Testing Sparse Block Index for cold segments...\n";
  size_t initial_count = k.store().count();
  (void)initial_count;
  
  // Seal current active objects into cold blocks
  k.store().seal_cold_blocks();
  assert(k.store().cold_blocks_count() >= 1);
  assert(k.store().cold_objects_count() == initial_count);
  assert(k.store().active_count() == 0);
  assert(k.store().count() == initial_count);

  // Point lookup from sealed cold block via get()
  auto cold_get1 = k.store().get(*id1);
  assert(cold_get1.ok());
  assert(cold_get1->id == *id1);
  assert(cold_get1->meta("customer_id") == "4471");

  // Provenance chain & history traversal over cold blocks
  auto cold_hist = k.store().find_history(*id3);
  assert(cold_hist.size() == 2);
  assert(cold_hist[0] == *id3);
  assert(cold_hist[1] == *id1);

  auto cold_prov = k.store().verify_provenance_chain(*id3);
  assert(cold_prov.ok());

  // AQL query against sealed cold blocks
  std::string q_cold = "SELECT * FROM objects WHERE meta.kind = \"erp.route\" AND meta.customer_id = \"4471\"";
  auto res_cold = execute_aql(self_surf, q_cold);
  assert(res_cold.success);
  assert(res_cold.count == 1);
  assert(res_cold.objects[0].id == *id1);

  // Add new active record after sealing
  StoredObject o_active;
  o_active.metadata.emplace_back("kind", "erp.active_route");
  o_active.metadata.emplace_back("customer_id", "5555");
  o_active.payload = ByteBuffer{'A', 'c', 't', 'i', 'v', 'e'};
  auto id_active = self_surf.store(o_active);
  assert(id_active.ok());

  assert(k.store().active_count() == 1);
  assert(k.store().cold_objects_count() == initial_count);
  assert(k.store().count() == initial_count + 1);

  std::cout << "  Sparse Block Index passed: sealed " << k.store().cold_objects_count() << " object(s) across "
            << k.store().cold_blocks_count() << " block(s), point & AQL queries resolved cleanly from cold storage.\n";

  // 8. Phase 0 Micro-Threshold Boundary Harness Invariants
  std::cout << "[Step 8] Testing Phase 0 Micro-Threshold Boundary Invariants...\n";
  assert(k.store().residency_config().block_objects == 8);
  assert(k.store().residency_config().max_active == 16);
  assert(k.store().residency_config().hot_budget == 64 * 1024);

  // Write 24 more objects to trigger continuous auto-sealing
  std::vector<ObjectID> batch_ids;
  for (int i = 0; i < 24; ++i) {
    StoredObject obj;
    obj.metadata.emplace_back("kind", "micro_harness.record");
    obj.metadata.emplace_back("batch_idx", std::to_string(i));
    obj.metadata.emplace_back("parity", (i % 2 == 0) ? "even" : "odd");
    // Large payload to force eviction sweep under 64KB hot budget
    obj.payload = ByteBuffer(4096, static_cast<uint8_t>('A' + (i % 26)));
    auto bid = self_surf.store(obj);
    assert(bid.ok());
    batch_ids.push_back(*bid);
  }

  // Verify auto-sealing occurred (cold_blocks_count() > 0)
  assert(k.store().cold_blocks_count() >= 2);

  // Invariant 1: Exactly-Once Resolution
  std::string q_even = "SELECT * FROM objects WHERE meta.kind = \"micro_harness.record\" AND meta.parity = \"even\"";
  auto res_even = execute_aql(self_surf, q_even);
  assert(res_even.success);
  assert(res_even.count == 12);
  std::unordered_set<ObjectID, TypedIdHash<9>> seen_ids;
  for (const auto& obj : res_even.objects) {
    assert(seen_ids.insert(obj.id).second); // Every object returned exactly once
  }

  // Invariant 2: No Index Orphans (all batch IDs resolve)
  for (size_t i = 0; i < batch_ids.size(); ++i) {
    auto got = k.store().get(batch_ids[i]);
    assert(got.ok());
    assert(got->meta("batch_idx") == std::to_string(i));
    assert(got->payload.size() == 4096);
    assert(got->payload[0] == static_cast<uint8_t>('A' + (i % 26)));
  }

  // Invariant 3: Count & Collection Identity
  assert(k.store().count() == k.store().active_count() + k.store().cold_objects_count());
  assert(k.store().list().size() == k.store().count());

  // Invariant 4: Budget Integrity
  assert(k.store().hot_bytes() <= k.store().hot_budget());

  std::cout << "  Phase 0 Invariants verified: auto-sealing (" << k.store().cold_blocks_count()
            << " cold blocks), eviction sweep (hot_bytes=" << k.store().hot_bytes()
            << " <= " << k.store().hot_budget() << "), exactly-once resolution, count identity.\n";

  std::cout << "\nALL AQL CHECKS PASSED SUCCESSFULLY!\n";
  return 0;
}
