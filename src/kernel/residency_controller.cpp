#include "aios/kernel/residency_controller.hpp"

namespace aios {

ResidencyController::ResidencyController(hal::Host& host, EntityManager& entities, ObjectStore& store)
    : host_(host), entities_(entities), store_(store) {}

ResidencyClass ResidencyController::evaluate(const EntityID& entity) const {
  hal::LockGuard g(mu_);
  const uint64_t now_ns = host_.monotonic_ns();

  // 1. Active execution graph / running transaction (entities().execution()),
  // or spatial focus. Both pin outright, so their relative order does not
  // matter; what matters is that BOTH come before pressure and recency, since
  // an entity being run or looked at must never be classified cold.
  const Entity* e = entities_.get(entity);
  if (e && !e->execution.is_null()) {
    return ResidencyClass::Pinned;
  }
  if (!focused_entity_.is_null() && focused_entity_ == entity) {
    return ResidencyClass::Pinned;
  }

  // 2. Data plane access recency (touched ObjectStore within N ms). This has
  // to be checked BEFORE pressure: an entity being actively read right now is
  // not a candidate for sealing just because the system is tight on memory —
  // sealing it would immediately force a rehydration on its very next access,
  // which is the thrashing case residency classing exists to prevent. Getting
  // this order backwards was caught by tools/residency-check.cpp's thrash
  // probe: pressure-before-recency labelled a just-touched entity Sealed on
  // 20 of 20 ticks.
  const uint64_t last_acc = store_.entity_last_access(entity);
  if (last_acc > 0 && (now_ns >= last_acc) && (now_ns - last_acc <= recency_window_ns_)) {
    return ResidencyClass::Warm;
  }

  // 3. System memory pressure (available_bytes() <= low_watermark). Only
  // reached once an entity is neither pinned nor recently touched.
  const uint64_t avail = host_.available_bytes();
  if (avail > 0 && avail <= low_watermark_bytes_) {
    return ResidencyClass::Sealed;
  }

  // 4. Spatial UI hints (unfocused dwell timeout)
  auto it = unfocused_dwell_start_ns_.find(entity);
  if (it != unfocused_dwell_start_ns_.end()) {
    if (now_ns >= it->second && (now_ns - it->second >= dwell_timeout_ns_)) {
      return ResidencyClass::Cool;
    }
  }

  return ResidencyClass::Warm;
}

void ResidencyController::step() {
  hal::LockGuard g(mu_);
  const uint64_t now_ns = host_.monotonic_ns();
  const uint64_t avail = host_.available_bytes();
  const bool under_pressure = (avail > 0 && avail <= low_watermark_bytes_);

  // Apply evaluation to all entities, in the same priority order as
  // evaluate(): execution/focus pin outright; recency is checked BEFORE
  // pressure, so an entity being read right now is never sealed out from
  // under itself and forced to rehydrate on its next access.
  auto ent_list = entities_.list();
  for (const auto& e : ent_list) {
    // 1. Active execution graph, or spatial focus — both pin.
    if (!e.execution.is_null()) {
      store_.set_residency_class(e.id, ResidencyClass::Pinned);
      continue;
    }
    if (!focused_entity_.is_null() && focused_entity_ == e.id) {
      store_.set_residency_class(e.id, ResidencyClass::Pinned);
      continue;
    }

    // 2. Data plane access recency
    const uint64_t last_acc = store_.entity_last_access(e.id);
    const bool recently_accessed = (last_acc > 0 && (now_ns >= last_acc) && (now_ns - last_acc <= recency_window_ns_));

    if (recently_accessed) {
      store_.set_residency_class(e.id, ResidencyClass::Warm);
      continue;
    }

    // 3. System memory pressure: Cool -> Sealed (Oldest First). Only reached
    // once an entity is neither pinned nor recently touched.
    if (under_pressure) {
      store_.set_residency_class(e.id, ResidencyClass::Sealed);
      continue;
    }

    // 4. Spatial UI hints: unfocused dwell timeout
    auto it = unfocused_dwell_start_ns_.find(e.id);
    if (it != unfocused_dwell_start_ns_.end() && (now_ns >= it->second) && (now_ns - it->second >= dwell_timeout_ns_)) {
      store_.set_residency_class(e.id, ResidencyClass::Cool);
    } else {
      store_.set_residency_class(e.id, ResidencyClass::Warm);
    }
  }

  if (under_pressure) {
    store_.seal_cold_blocks();
  }
}

void ResidencyController::on_focus_change(const EntityID& entity, bool focused) {
  hal::LockGuard g(mu_);
  const uint64_t now_ns = host_.monotonic_ns();
  if (focused) {
    focused_entity_ = entity;
    focus_timestamp_ns_ = now_ns;
    unfocused_dwell_start_ns_.erase(entity);
    store_.pin(entity);
  } else {
    if (focused_entity_ == entity) {
      focused_entity_ = EntityID{};
    }
    unfocused_dwell_start_ns_[entity] = now_ns;
    // Data Plane Invariant: Do not evict underlying ObjectStore records on blur unless system-wide memory pressure demands it.
    // Evaluate non-spatial activity first
    const Entity* e = entities_.get(entity);
    if (e && !e->execution.is_null()) {
      store_.set_residency_class(entity, ResidencyClass::Pinned);
    } else {
      const uint64_t last_acc = store_.entity_last_access(entity);
      if (last_acc > 0 && (now_ns >= last_acc) && (now_ns - last_acc <= recency_window_ns_)) {
        store_.set_residency_class(entity, ResidencyClass::Warm);
      } else {
        // Demote to Warm or Cool, preserving records in store
        store_.set_residency_class(entity, ResidencyClass::Cool);
      }
    }
  }
}

void ResidencyController::set_focused_entity(const EntityID& entity) {
  on_focus_change(entity, true);
}

EntityID ResidencyController::focused_entity() const {
  hal::LockGuard g(mu_);
  return focused_entity_;
}

void ResidencyController::set_low_watermark_bytes(uint64_t bytes) {
  hal::LockGuard g(mu_);
  low_watermark_bytes_ = bytes;
}

uint64_t ResidencyController::low_watermark_bytes() const {
  hal::LockGuard g(mu_);
  return low_watermark_bytes_;
}

void ResidencyController::set_recency_window_ns(uint64_t ns) {
  hal::LockGuard g(mu_);
  recency_window_ns_ = ns;
}

uint64_t ResidencyController::recency_window_ns() const {
  hal::LockGuard g(mu_);
  return recency_window_ns_;
}

void ResidencyController::set_dwell_timeout_ns(uint64_t ns) {
  hal::LockGuard g(mu_);
  dwell_timeout_ns_ = ns;
}

uint64_t ResidencyController::dwell_timeout_ns() const {
  hal::LockGuard g(mu_);
  return dwell_timeout_ns_;
}

bool ResidencyController::is_under_pressure() const {
  const uint64_t avail = host_.available_bytes();
  hal::LockGuard g(mu_);
  return (avail > 0 && avail <= low_watermark_bytes_);
}

}  // namespace aios
