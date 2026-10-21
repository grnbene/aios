#pragma once
// The C++ face of the Intelligence ABI. The kernel holds an Intelligence per
// bound entity and drives it through perceive → think → act. Implementations
// live OUTSIDE the kernel: a plugin, an external process, a native model.
#include <memory>
#include <string>
#include "aios/core/ids.hpp"
#include "aios/core/result.hpp"
#include "aios/state/state.hpp"
#include "aios/compute/compute_graph.hpp"
#include "aios/compute/scheduler.hpp"

namespace aios {

class Surface;

class Intelligence {
 public:
  virtual ~Intelligence() = default;
  virtual std::string name() const = 0;
  // Ingest the entity's context; return the state the intelligence now holds.
  virtual Result<StateID> perceive(Surface& s, const StateObject& context) = 0;
  // Produce the computation the kernel should schedule. Null = nothing to compute.
  virtual Result<GraphID> think(Surface& s, const StateObject& perceived) = 0;
  // Produce the next state from what was computed.
  virtual Result<StateID> act(Surface& s, const StateObject& perceived, const ComputeGraph* graph, const ExecutionReport* report) = 0;
};

// Loads a shared object exporting aios_intelligence_create.
Result<std::unique_ptr<Intelligence>> load_intelligence_plugin(const std::string& path);
// Binds an out-of-kernel intelligence reached over the frame protocol on a
// child process's stdio. The kernel is unaware of this arrangement.
Result<std::unique_ptr<Intelligence>> spawn_external_intelligence(const std::vector<std::string>& argv, uint64_t timeout_ms = 600000);
// The same, with environment for the peer: each "NAME=value" is set over the
// environment this process has, for the peer and for any respawn of it. How a
// service gives each agent its own model (AIOS_MODEL_PROVIDER, AIOS_MODEL, ...)
// without touching its own environment, which every other agent shares.
Result<std::unique_ptr<Intelligence>> spawn_external_intelligence(const std::vector<std::string>& argv, const std::vector<std::string>& env, uint64_t timeout_ms = 600000);
// An out-of-kernel intelligence on a wire: the frame protocol over a serial
// line the substrate provides (bare metal: the second UART). Whatever sits at
// the other end — a model behind the Assay bridge, a person with a keyboard —
// is the mind.
Result<std::unique_ptr<Intelligence>> make_wire_intelligence();
// A deterministic in-kernel reference: reflects state back, computes nothing.
std::unique_ptr<Intelligence> make_echo_intelligence();

}  // namespace aios
