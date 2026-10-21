#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include "aios/core/ids.hpp"
#include "aios/entity/entity.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/sync.hpp"
#include "aios/storage/object_store.hpp"

namespace aios {

class EntityManager;

class ResidencyController {
 public:
  ResidencyController(hal::Host& host, EntityManager& entities, ObjectStore& store);
  ~ResidencyController() = default;

  // Evaluation order per Roadmap (v2):
  // 1. Active execution graph / running transaction (entities().execution()).
  // 2. Data plane access recency (touched ObjectStore within N ms).
  // 3. System memory pressure (available_bytes() <= low_watermark).
  // 4. Spatial UI hints (unfocused dwell timeout).
  ResidencyClass evaluate(const EntityID& entity) const;

  // Run full evaluation cycle over all known entities and apply pressure policies
  void step();
  void update() { step(); }

  // Compositor focus signals
  void on_focus_change(const EntityID& entity, bool focused);
  void set_focused_entity(const EntityID& entity);
  EntityID focused_entity() const;

  // Pressure and timing parameters
  void set_low_watermark_bytes(uint64_t bytes);
  uint64_t low_watermark_bytes() const;
  void set_recency_window_ns(uint64_t ns);
  uint64_t recency_window_ns() const;
  void set_dwell_timeout_ns(uint64_t ns);
  uint64_t dwell_timeout_ns() const;

  bool is_under_pressure() const;

 private:
  hal::Host& host_;
  EntityManager& entities_;
  ObjectStore& store_;

  mutable hal::Mutex mu_;
  EntityID focused_entity_;
  uint64_t focus_timestamp_ns_ = 0;
  std::unordered_map<EntityID, uint64_t, TypedIdHash<1>> unfocused_dwell_start_ns_;

  uint64_t recency_window_ns_ = 5000000000ull;          // 5 seconds
  uint64_t dwell_timeout_ns_ = 3000000000ull;            // 3 seconds
  uint64_t low_watermark_bytes_ = 64ull * 1024 * 1024;  // 64 MB default
};

}  // namespace aios
