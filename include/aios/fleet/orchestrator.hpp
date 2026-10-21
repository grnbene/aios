#pragma once
// The placement brain. Migration is the muscle — this decides when to use it.
// One orchestrator runs per kernel. Each tick it samples its own load and polls
// its connected peers, then acts on its own authority:
//   - rebalance: if this node is oversubscribed and a peer has headroom, move a
//                job to the least-loaded peer.
//   - recover:   if a peer this node holds a recovery point for has dropped, the
//                job reappears here from its last replicated checkpoint.
// Every decision — including the decision to do nothing — lands on a log, so the
// brain always shows its work. The orchestrator owns no policy the kernel does
// not enforce: a migration still goes through Kernel::migrate (capabilities,
// hard walls, identity preservation), a recovery through restore_image.
#include <vector>
#include <string>
#include "aios/core/ids.hpp"
#include "aios/core/result.hpp"
#include "aios/fleet/telemetry.hpp"

namespace aios {

class Kernel;

struct PeerView {
  size_t index;            // position in the kernel's node list
  std::string address;
  NodeTelemetry tele;
  bool reachable = false;  // telemetry poll succeeded this pass
};

struct PlacementDecision {
  enum Kind { Hold, Rebalance, Recover } kind = Hold;
  uint64_t ns = 0;
  EntityID entity;         // the job the decision is about (null for a fleet-wide Hold)
  Id128 from;              // kernel it left (or would leave)
  Id128 to;                // kernel it lands on
  double from_pressure = 0, to_pressure = 0;
  std::string reason;
  bool executed = false;   // tick() executed it; plan() leaves it false
  const char* kind_name() const {
    switch (kind) { case Rebalance: return "rebalance"; case Recover: return "recover"; default: return "hold"; }
  }
};

class Orchestrator {
 public:
  explicit Orchestrator(Kernel& k);

  // Watermarks (fractions of capacity). Migrate when local pressure >= high and a
  // peer is below low + has a free slot. Hysteresis (high>low) stops thrashing.
  // Refused (false, nothing changed) unless 0 <= low < high <= 1.
  bool set_watermarks(double high, double low) {
    if (!(low >= 0.0 && high <= 1.0 && low < high)) return false;
    high_ = high; low_ = low; return true;
  }
  double high() const { return high_; }
  double low() const { return low_; }

  NodeTelemetry local() const;                 // this kernel, right now
  std::vector<PeerView> survey();              // poll every connected peer (opcode Telemetry)

  std::vector<PlacementDecision> plan();               // what a tick would do — no side effects
  std::vector<PlacementDecision> tick();               // one autonomous pass, decisions executed
  std::vector<PlacementDecision> rebalance(bool execute = true); // migrate queued jobs from oversubscribed node to idle peers

  // Ship recovery points for this node's movable jobs to a standby peer, so a
  // hard death is recoverable off-box. Returns how many were shipped.
  Result<size_t> replicate_all(size_t standby_index);

  const std::vector<PlacementDecision>& log() const { return log_; }

 private:
  // Candidate jobs the scheduler may move: alive, non-resident, not a peer's
  // guest session, not the kernel itself.
  std::vector<EntityID> movable_jobs() const;
  std::vector<PlacementDecision> decide(bool execute);

  Kernel& k_;
  std::vector<PlacementDecision> log_;
  double high_ = 1.0;   // at/over slot capacity
  double low_ = 0.75;   // a peer must be this far below capacity to receive
};

}  // namespace aios
