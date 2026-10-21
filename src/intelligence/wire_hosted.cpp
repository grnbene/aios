// On the Linux substrate there is no dedicated wire: an external mind is a
// child process (spawn_external_intelligence). Bare metal implements the wire.
#include "aios/intelligence/intelligence.hpp"
namespace aios {
Result<std::unique_ptr<Intelligence>> make_wire_intelligence() {
  return Result<std::unique_ptr<Intelligence>>(Status::Unsupported, "no wire on this substrate; bind an external mind with: mind bind <e> extern <cmd...>");
}
}  // namespace aios
