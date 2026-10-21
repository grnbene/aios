// The reference intelligence: deterministic, in-process, computes nothing.
// It exists so the cognitive cycle can be driven without any external mind.
#include "aios/intelligence/intelligence.hpp"
#include "aios/kernel/surface.hpp"

namespace aios {
namespace {
class EchoIntelligence final : public Intelligence {
 public:
  std::string name() const override { return "echo"; }
  Result<StateID> perceive(Surface& s, const StateObject& context) override {
    return s.create_state(StateKind::Context, context.payload);
  }
  Result<GraphID> think(Surface&, const StateObject&) override { return GraphID{}; }
  Result<StateID> act(Surface& s, const StateObject& perceived, const ComputeGraph*, const ExecutionReport*) override {
    std::string text = "echo(" + std::to_string(perceived.payload.size()) + " bytes): " + string_of(perceived.payload);
    return s.create_state(StateKind::Working, span_of(text));
  }
};
}  // namespace
std::unique_ptr<Intelligence> make_echo_intelligence() { return std::make_unique<EchoIntelligence>(); }
}  // namespace aios
