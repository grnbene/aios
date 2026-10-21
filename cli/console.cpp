#include "cli.hpp"
#include "aios/intelligence/intelligence.hpp"
#include "aios/intelligence/qwen_mind.hpp"
#include <cstdlib>

namespace aios::cli {

AuthorityProfile console_authority() {
  using K = ResourceKind;
  return {
    // Kernel ACCESS: see it, operate the machine beneath it, checkpoint,
    // schedule, and delegate those. Not write, destroy, allocate, publish or
    // consume over the kernel — ownership stays with the kernel entity.
    {ResourceRef{K::Kernel, {}},        (uint16_t)(R_Read | R_Execute | R_Checkpoint | R_Schedule | R_Grant), 0},
    // Entities: create them, act as them, end them, and pass those on.
    {ResourceRef{K::Entity, {}},        (uint16_t)(R_Read | R_Allocate | R_Execute | R_Destroy | R_Grant), 0},
    // Memory: a large but finite budget, fully delegable within it.
    {ResourceRef{K::Memory, {}},        (uint16_t)(R_Read | R_Write | R_Allocate | R_Destroy | R_Grant), 4ull << 30},
    {ResourceRef{K::State, {}},         (uint16_t)(R_Read | R_Write | R_Allocate | R_Destroy | R_Grant), 0},
    {ResourceRef{K::Channel, {}},       (uint16_t)(R_Allocate | R_Publish | R_Consume | R_Destroy | R_Grant), 0},
    {ResourceRef{K::ComputeDevice, {}}, (uint16_t)(R_Execute | R_Schedule | R_Grant), 0},
    {ResourceRef{K::StorageObject, {}}, (uint16_t)(R_Read | R_Write | R_Grant), 0},
    {ResourceRef{K::Network, {}},       (uint16_t)(R_Publish | R_Consume | R_Grant), 0},
  };
}

// The external mind this machine is configured to run: node on the assay
// bridge. A machine kept alive by a supervisor has no useful working
// directory, so the relative candidates can only find the bridge when someone
// launched the surface by hand from the source tree. AIOS_BRIDGE is what makes
// a resident machine come back with the same mind it had.
// One KEY from the machine's own settings file. A resident started by a
// supervisor has no useful working directory and a sanitised environment, so
// the settings file is the only place it can be told where things are.
std::string machine_conf(Session& s, const std::string& key) {
  std::string path;
  if (const char* c = std::getenv("AOS_CONF")) path = c;
  if (path.empty()) {
    const char* home = std::getenv("HOME");
    if (!home) return {};
    path = std::string(home) + "/.config/aos.conf";
  }
  auto text = s.kernel->host().read_file(path);
  if (!text.ok()) return {};
  std::string body(reinterpret_cast<const char*>(text->data()), text->size());
  size_t at = 0;
  while (at < body.size()) {
    size_t nl = body.find('\n', at);
    std::string line = body.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
    at = (nl == std::string::npos) ? body.size() : nl + 1;
    size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos || line[b] == '#') continue;
    size_t eq = line.find('=', b);
    if (eq == std::string::npos) continue;
    if (line.substr(b, eq - b) != key) continue;
    std::string v = line.substr(eq + 1);
    size_t vb = v.find_first_not_of(" \t");
    size_t ve = v.find_last_not_of(" \t\r");
    if (vb == std::string::npos) return {};
    return v.substr(vb, ve - vb + 1);
  }
  return {};
}

Result<std::unique_ptr<Intelligence>> spawn_bridge_mind(Session& s) {
  std::string bridge;
  if (const char* named = std::getenv("AIOS_BRIDGE")) bridge = named;
  if (bridge.empty() || !s.kernel->host().exists(bridge)) {
    std::string from_conf = machine_conf(s, "AOS_BRIDGE");
    if (!from_conf.empty()) bridge = from_conf;
  }
  if (bridge.empty() || !s.kernel->host().exists(bridge)) {
    bridge = "intelligence/assay/bridge.cjs";
    if (!s.kernel->host().exists(bridge)) {
      bridge = s.kernel->config().root + "/../intelligence/assay/bridge.cjs";
    }
  }
  if (!s.kernel->host().exists(bridge))
    return Result<std::unique_ptr<Intelligence>>(Status::NotFound,
        "no assay bridge on this machine; set AIOS_BRIDGE to bridge.cjs");
  return spawn_external_intelligence({"node", bridge});
}

Result<void> auto_bind_mind(Session& s, const std::string& label) {
  if (!s.kernel || !s.me) return Result<void>(Status::Invalid, "no kernel or console entity");
  EntityID mind_id;
  for (auto& r : s.kernel->authority().residents()) if (r.label == label) mind_id = r.id;
  if (mind_id.is_null()) return Result<void>(Status::NotFound, "no resident entity named '" + label + "'");

  if (s.kernel->mind(mind_id)) return Ok();

  const char* env_kind = std::getenv("AIOS_MIND_KIND");
  const char* env_url = std::getenv("AIOS_MIND_URL");
  const char* env_model = std::getenv("AIOS_MIND_MODEL");
  const char* env_path = std::getenv("AIOS_MIND_PATH");

  std::string kind = env_kind ? env_kind : "";
  std::string url = env_url ? env_url : "";   // no endpoint is built in
  std::string model = env_model ? env_model : "spark";

  if (kind == "local" || kind == "cuda" || kind == "local-cuda") {
    return Result<void>(Status::Unsupported,
        "AIOS_MIND_KIND=" + kind + " named a mind that ran no model and has been removed; "
        "set AIOS_MIND_KIND=qwen with AIOS_MIND_URL, or leave it unset to use the bridge");
  }
  // Only bind the OpenAI-shaped HTTP mind when someone actually asked for it.
  // This used to read `|| kind.empty()`, so a default boot -- which is every
  // boot, nothing sets AIOS_MIND_KIND -- bound qwen at a hardcoded LAN address
  // and reported "qwen:spark" on the glass. Every branch below it, the bridge
  // included, was unreachable code.
  if (kind == "qwen" || kind == "spark" || kind == "llm" || env_url != nullptr) {
    if (url.empty())
      return Result<void>(Status::Invalid, "AIOS_MIND_KIND=" + kind + " needs AIOS_MIND_URL (e.g. http://<host>:8000/v1/chat/completions); no endpoint is built in");
    return s.me->bind_mind(mind_id, intelligence::make_qwen_intelligence(url, env_model ? env_model : "spark"));
  }
  if (kind == "plugin" && env_path != nullptr) {
    auto plug = load_intelligence_plugin(env_path);
    if (plug.ok()) return s.me->bind_mind(mind_id, std::move(*plug));
  }

  auto wire = make_wire_intelligence();
  if (wire.ok()) {
    return s.me->bind_mind(mind_id, std::move(*wire));
  }

  auto ext = spawn_bridge_mind(s);
  if (ext.ok()) return s.me->bind_mind(mind_id, std::move(*ext));

  auto echo = make_echo_intelligence();
  if (echo) {
    return s.me->bind_mind(mind_id, std::move(echo));
  }
  return Ok();
}

Result<void> hot_swap_mind(Session& s, const std::string& label) {
  if (!s.kernel || !s.me) return Result<void>(Status::Invalid, "no kernel or console entity");
  EntityID mind_id;
  for (auto& r : s.kernel->authority().residents()) if (r.label == label) mind_id = r.id;
  if (mind_id.is_null()) return Result<void>(Status::NotFound, "no resident entity named '" + label + "'");

  const char* env_kind = std::getenv("AIOS_MIND_KIND");
  const char* env_url = std::getenv("AIOS_MIND_URL");
  const char* env_model = std::getenv("AIOS_MIND_MODEL");
  const char* env_path = std::getenv("AIOS_MIND_PATH");

  std::string kind = env_kind ? env_kind : "";
  std::string url = env_url ? env_url : "";   // no endpoint is built in
  std::string model = env_model ? env_model : "spark";

  std::unique_ptr<Intelligence> new_mind;
  if (kind == "qwen" || kind == "spark" || kind == "llm" || env_url != nullptr) {
    if (url.empty())
      return Result<void>(Status::Invalid, "AIOS_MIND_KIND=" + kind + " needs AIOS_MIND_URL (e.g. http://<host>:8000/v1/chat/completions); no endpoint is built in");
    new_mind = intelligence::make_qwen_intelligence(url, env_model ? env_model : "spark");
  } else if (kind == "plugin" && env_path != nullptr) {
    auto plug = load_intelligence_plugin(env_path);
    if (plug.ok()) new_mind = std::move(*plug);
  } else if (kind == "echo") {
    new_mind = make_echo_intelligence();
  }

  if (!new_mind) {
    auto wire = make_wire_intelligence();
    if (wire.ok()) new_mind = std::move(*wire);
  }
  if (!new_mind) {
    // The bridge, which is what this machine is actually configured to run.
    // hot_swap could never reach it before: it went wire -> the removed fake
    // CUDA mind -> echo, and the fake always succeeded.
    auto ext = spawn_bridge_mind(s);
    if (ext.ok()) new_mind = std::move(*ext);
  }
  if (!new_mind) {
    new_mind = make_echo_intelligence();
  }

  if (new_mind) {
    return s.me->swap_mind(mind_id, std::move(new_mind));
  }
  return Result<void>(Status::Invalid, "failed to create dynamic replacement mind");
}

Result<void> bind_console(Session& s, const std::string& label) {
  if (!s.kernel) return Result<void>(Status::Invalid, "no kernel");
  auto id = s.kernel->resident(label, console_authority(), /*self_improve=*/true);
  if (!id.ok()) return id.error();
  s.me = std::make_unique<Surface>(s.kernel->as(*id));
  // The resident mind: the entity `ask` speaks to. A privileged resident like
  // the console itself — kernel access, never kernel ownership — persistent,
  // and bound to an intelligence with `mind bind mind ...`.
  auto m = s.kernel->resident("mind", console_authority(), /*self_improve=*/true);
  if (!m.ok()) return m.error();
  auto_bind_mind(s, "mind");
  return Ok();
}

}  // namespace aios::cli
