/* The real-STL half of the "entity" shell command - kept out of main.cpp on
 * purpose. main.cpp compiles with the strict freestanding flags (-nostdinc++
 * among them) the rest of metal/arm64 needs; this file compiles with
 * STLFLAGS instead (see build-arm64.sh) so it can use the real
 * aios/entity/entity.hpp and the STL it's built on. A single extern "C"
 * function is the entire seam between the two worlds. */
#include "aios/entity/entity.hpp"
#include "aios/core/ids.hpp"
#include "kio.hpp"

extern "C" void entity_test_command() {
  aios::IdGenerator ids(0x9E3779B97F4A7C15ull, 0);
  aios::EntityManager mgr(ids);

  auto created = mgr.create("arm64-entity-test", 0);
  if (!created.ok()) {
    kprintf("  entity: create() failed: %s\n", created.error().str().c_str());
    return;
  }
  const aios::EntityID id = *created;

  kprintf("  entity: created id=%s label=arm64-entity-test\n", id.short_str().c_str());
  kprintf("  entity: count()=%llu\n", static_cast<unsigned long long>(mgr.count()));

  const aios::Entity* e = mgr.get(id);
  if (!e) { kputs("  entity: get() returned null - FAIL\n"); return; }
  kprintf("  entity: get() label='%s' status=%s\n", e->label.c_str(), aios::entity_status_name(e->status));

  auto st = mgr.set_status(id, aios::EntityStatus::Active);
  if (!st.ok()) { kprintf("  entity: set_status failed: %s\n", st.error().str().c_str()); return; }
  e = mgr.get(id);
  kprintf("  entity: after set_status, status=%s\n", e ? aios::entity_status_name(e->status) : "?");

  auto removed = mgr.remove(id);
  kprintf("  entity: remove() -> %s, count now %llu\n", removed.ok() ? "ok" : "FAIL",
          static_cast<unsigned long long>(mgr.count()));

  kputs("  entity: real STL (std::string, std::vector, std::unordered_map) "
        "running end to end on ARM64 bare metal.\n");
}
