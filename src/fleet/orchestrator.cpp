// The placement brain in motion. A tick is one honest pass: look at my own
// load, ask each peer for theirs, then act — move a job off an oversubscribed
// node, or bring a dead node's job back from its recovery point. Every pass
// records what it decided, including deciding to hold.
#include "aios/fleet/orchestrator.hpp"
#include "aios/kernel/kernel.hpp"

namespace aios {

Orchestrator::Orchestrator(Kernel& k) : k_(k) {}

NodeTelemetry Orchestrator::local() const { return k_.telemetry(); }

std::vector<EntityID> Orchestrator::movable_jobs() const {
  std::vector<EntityID> out;
  for (const auto& e : k_.entities().list()) {
    if (e.id == k_.self()) continue;
    if (k_.is_resident(e.id)) continue;
    if (e.status == EntityStatus::Terminated) continue;
    if (e.label.rfind("peer:", 0) == 0) continue;   // a peer's guest session, not our job
    out.push_back(e.id);
  }
  return out;
}

std::vector<PeerView> Orchestrator::survey() {
  std::vector<PeerView> views;
  const auto& nodes = k_.nodes();
  for (size_t i = 0; i < nodes.size(); ++i) {
    PeerView v; v.index = i; v.address = nodes[i]->address();
    auto t = nodes[i]->telemetry();
    if (t.ok()) { v.tele = *t; v.reachable = true; }
    else { v.tele.kernel = nodes[i]->kernel(); v.reachable = false; }
    views.push_back(v);
  }
  return views;
}

// A source kernel is considered dead here when it once connected to us (to hand
// over its recovery points) and that session has since dropped. This is the one
// unambiguous death signal a standby has: the node that gave me the work is gone.
static bool source_dropped(Kernel& k, const Id128& source) {
  if (!k.service()) return false;
  for (const auto& p : k.service()->peers())
    if (p.kernel == source && !p.connected) return true;
  return false;
}

std::vector<PlacementDecision> Orchestrator::decide(bool execute) {
  std::vector<PlacementDecision> decisions;
  const uint64_t now = k_.now();
  // If we are in a fleet, discover and connect any new peers before deciding —
  // the scheduler sees the whole roster, not a hand-wired peer list.
  k_.fleet_sync();
  NodeTelemetry L = local();
  auto peers = survey();

  // 1) Recovery has priority: a dead node's work should reappear before we
  //    fuss over balance. Restore every recovery point whose owner has dropped.
  for (const auto& rep : k_.replicas()) {
    if (!source_dropped(k_, rep.source_kernel)) continue;
    if (k_.entities().exists(rep.entity)) continue;   // already back
    PlacementDecision p; p.kind = PlacementDecision::Recover; p.ns = now; p.entity = rep.entity;
    p.from = rep.source_kernel; p.to = L.kernel; p.from_pressure = 0; p.to_pressure = L.pressure();
    if (execute) {
      auto r = k_.recover_replica(rep.entity);
      p.executed = r.ok();
      p.reason = r.ok() ? ("source " + rep.source_kernel.short_str() + " dropped; job restored here from its last recovery point")
                        : ("recovery failed: " + r.error().message);
    } else {
      p.reason = "source " + rep.source_kernel.short_str() + " dropped; would restore job here from recovery point";
    }
    decisions.push_back(p);
    if (execute && p.executed) log_.push_back(p);
    L = local();  // recovering added a job here; re-read before balancing
  }

  // 2) Rebalance: if I am oversubscribed and a peer has headroom, move one job
  //    to the least-loaded peer. One job per pass — draining is a sequence of
  //    small, inspectable moves, not one opaque shuffle.
  auto reb = rebalance(execute);
  if (!reb.empty()) {
    decisions.insert(decisions.end(), reb.begin(), reb.end());
    return decisions;   // one move; the next tick reassesses from the new state
  }

  // 3) Nothing to do — say why, so a quiet fleet is legible.
  {
    PlacementDecision p; p.kind = PlacementDecision::Hold; p.ns = now; p.from = L.kernel; p.to = L.kernel;
    p.from_pressure = p.to_pressure = L.pressure();
    if (L.pressure() >= high_) p.reason = "oversubscribed but no peer is at or under low-water " + std::to_string(low_).substr(0,4) + " with a free slot — holding";
    else p.reason = "pressure " + std::to_string(L.pressure()).substr(0,4) + " under high-water " + std::to_string(high_).substr(0,4) + " — placed";
    decisions.push_back(p);
    if (execute) log_.push_back(p);
  }
  return decisions;
}

std::vector<PlacementDecision> Orchestrator::plan() { return decide(false); }
std::vector<PlacementDecision> Orchestrator::tick() { return decide(true); }

std::vector<PlacementDecision> Orchestrator::rebalance(bool execute) {
  std::vector<PlacementDecision> decisions;
  const uint64_t now = k_.now();
  NodeTelemetry L = local();
  auto peers = survey();

  if (L.pressure() >= high_ && L.free_slots() < 0) {
    const PeerView* best = nullptr;
    for (const auto& v : peers) {
      if (!v.reachable || v.tele.free_slots() <= 0) continue;
      if (v.tele.pressure() > low_) continue;                  // over the low watermark: no room to receive
      if (v.tele.pressure() >= L.pressure()) continue;         // not actually less loaded
      if (best == nullptr || v.tele.pressure() < best->tele.pressure()) best = &v;
    }
    auto jobs = movable_jobs();
    if (best && !jobs.empty()) {
      EntityID job = jobs.front();
      PlacementDecision p; p.kind = PlacementDecision::Rebalance; p.ns = now; p.entity = job;
      p.from = L.kernel; p.to = best->tele.kernel;
      p.from_pressure = L.pressure(); p.to_pressure = best->tele.pressure();
      if (execute) {
        auto r = k_.migrate(job, best->index);
        p.executed = r.ok();
        p.reason = r.ok() ? ("oversubscribed (" + std::to_string(L.movable_entities) + "/" + std::to_string(L.slots) +
                             " slots); moved to peer with " + std::to_string(best->tele.free_slots()) + " free")
                          : ("migration failed: " + r.error().message);
      } else {
        p.reason = "oversubscribed; would move to " + best->address + " (" + std::to_string(best->tele.free_slots()) + " free)";
      }
      decisions.push_back(p);
      if (execute && p.executed) log_.push_back(p);
      return decisions;   // one move; the next tick reassesses from the new state
    }
  }
  return decisions;
}

Result<size_t> Orchestrator::replicate_all(size_t standby_index) {
  if (standby_index >= k_.nodes().size())
    return Result<size_t>(Status::NotFound, "no peer node #" + std::to_string(standby_index));
  auto& node = k_.nodes()[standby_index];
  size_t shipped = 0;
  for (const auto& job : movable_jobs()) {
    auto img = k_.capture(job);
    if (!img.ok()) return img.error();
    const Entity* e = k_.entities().get(job);
    uint64_t gen = e ? e->generation : 0;
    auto r = node->replicate(job, gen, *img);
    if (!r.ok()) return r.error();
    ++shipped;
  }
  return shipped;
}

}  // namespace aios
