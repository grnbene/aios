// Agent Control Service — a sidecar that lets an external (non-aios) process
// create a capability-scoped agent entity, bind a real Intelligence to it,
// grant/attenuate/revoke its authority, drive perceive->think->act cycles,
// and read back the audit ledger of exactly what it was allowed to do and
// did. Same deployment shape as aios-server: hosted Linux, no bare-metal
// work, one more systemd unit on a box that's already running one.
//
// Phase 1 of the Agent Control Service plan: boot the kernel, create the
// service's own resident identity with a deliberately narrow starting
// authority, and stop there. No wire protocol yet -- that's Phase 2. This
// file's only job right now is to prove the resident boots cleanly and
// holds exactly the authority it's supposed to, nothing more.
#undef NDEBUG
#include <iostream>
#include <string>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdlib>

#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/capability/capability.hpp"
#include "aios/channel/agent_control_serve.hpp"
#include "aios/hal/sync.hpp"
#include <fstream>
#include <map>
#include <sstream>

using namespace aios;

static std::atomic<bool> g_shutdown{false};

static void handle_signal(int) {
  g_shutdown = true;
}

// The service's own starting authority. Every wire op in Phase 2 operates
// through the Surface this profile backs -- Surface::grant/request can only
// ever delegate FROM what's granted here (never Kernel::grant, which mints
// unattenuated). Deliberately narrower than console_authority()
// (cli/console.cpp:8): this is a headless sidecar handing scoped authority
// to short-lived agent entities, not an interactive operator console.
// Widen a line here ONLY when a specific Phase 2/5 step proves it's needed,
// and say why, the same way console_authority() documents each grant.
static uint64_t env_u64(const char* name, uint64_t fallback) {
  const char* v = std::getenv(name);
  if (!v || !*v) return fallback;
  try { return std::stoull(v); } catch (...) { return fallback; }
}

static AuthorityProfile agent_control_service_authority() {
  using K = ResourceKind;
  const uint64_t memory_quota = env_u64("AIOS_AGENT_CONTROL_MEMORY_MB", 256) << 20;
  return {
    // Entities: create agent entities, act as them to run their cycles,
    // terminate them, and delegate rights over them -- but never over
    // ourselves or the kernel entity.
    {ResourceRef{K::Entity, {}},        (uint16_t)(R_Read | R_Allocate | R_Execute | R_Destroy | R_Grant), 0},
    // Kernel: see it, re-derive kernel-scoped grants for bookkeeping, and
    // Schedule -- proven necessary, not assumed: Scheduler::execute()
    // (src/compute/scheduler.cpp:85) requires Kernel:Schedule on the
    // EXECUTING ENTITY itself before any compute graph runs, machine-wide,
    // regardless of device. Without this the service could never delegate
    // it onward, so an agent asking its own bound mind to actually compute
    // something (not just chat) would fail every single time -- proven by
    // running exactly that and watching it fail clean with this error.
    // Checkpoint, for agent.checkpoint / agent.rewind: an agent's work is
    // recorded cycle by cycle and an operator can send it back to the start of
    // one (Surface::checkpoint_of needs kernel:checkpoint). Still no
    // Write/Destroy/Execute over the kernel object itself -- this service
    // operates agents' compute, not the machine.
    {ResourceRef{K::Kernel, {}},        (uint16_t)(R_Read | R_Grant | R_Schedule | R_Checkpoint), 0},
    // Memory: a real but bounded budget per the service's own quota, fully
    // delegable within it to whatever agents it spawns. Deliberately
    // smaller than the console's 4GiB default -- many short-lived agents,
    // not one long-lived operator session.
    {ResourceRef{K::Memory, {}},        (uint16_t)(R_Read | R_Write | R_Allocate | R_Destroy | R_Grant), memory_quota},
    // State: every perceive/think/act cycle creates State objects (context,
    // perceived, result) -- an agent needs this to run at all.
    {ResourceRef{K::State, {}},         (uint16_t)(R_Read | R_Write | R_Allocate | R_Destroy | R_Grant), 0},
    // Channel: for bind_mind's optional output channel.
    {ResourceRef{K::Channel, {}},       (uint16_t)(R_Allocate | R_Publish | R_Consume | R_Destroy | R_Grant), 0},
    // ComputeDevice: Surface::cycle's internal execute() path needs this
    // even for a mind whose think() returns no graph (QwenIntelligence
    // does today) -- kept minimal. StorageObject and Network follow below,
    // each added for one proven need. Widen only when a specific op proves it's needed.
    {ResourceRef{K::ComputeDevice, {}}, (uint16_t)(R_Execute | R_Schedule | R_Grant), 0},
    // StorageObject: a coding agent reads its repository tree and writes the paths it has claimed, and the caller
    // that drives it (converge's runtime) grants it exactly those: cap.grant storage "tree:<repo>" read and
    // "path:<repo>/<file>" read,write. Without this every such grant is refused and the agent ends before its first
    // action. Read, Write and Grant only: no Allocate/Destroy (it creates and deletes no storage objects), and the
    // files themselves are fenced by the caller's sandbox, which this authority does not replace.
    {ResourceRef{K::StorageObject, {}}, (uint16_t)(R_Read | R_Write | R_Grant), 0},
    // Network: the same agent pushes its work to a git remote and calls the platform's tools, each granted as
    // Publish on a named endpoint ("git:<remote>", "converge:<url>"). Publish and Grant only: nothing here lets
    // an agent consume from, or open, a connection of its own.
    {ResourceRef{K::Network, {}},       (uint16_t)(R_Publish | R_Grant), 0},
  };
}

int main(int argc, char** argv) {
  // Unbuffered: this runs under systemd (piped, not a tty), and a killed
  // or piped process must never lose its last log lines to a stdio buffer
  // that never got flushed -- the same lesson the auto-vacuum loop in
  // aios-server.cpp learned the hard way.
  std::cout << std::unitbuf;
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

#if defined(_WIN32)
  // "/tmp/..." resolves to the current drive's root on Windows, which is
  // writable only by an administrator on most installs; %TEMP% is this
  // account's own, same as every other Windows CLI tool defaults to.
  const char* win_tmp = std::getenv("TEMP");
  std::string root_dir = (win_tmp && *win_tmp) ? std::string(win_tmp) + "\\aios-agent-control-data" : "aios-agent-control-data";
#else
  std::string root_dir = "/tmp/aios-agent-control-data";
#endif
  uint16_t port = 7312;
  bool loopback_only = true;
  std::string auth_secret_file;
  std::map<std::string, std::string> external_minds;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--root" && i + 1 < argc) {
      root_dir = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      port = (uint16_t)std::stoi(argv[++i]);
    } else if (arg == "--listen-all") {
      loopback_only = false;
    } else if (arg == "--auth-secret-file" && i + 1 < argc) {
      auth_secret_file = argv[++i];
    } else if (arg == "--external-mind" && i + 1 < argc) {
      // name=command: a mind agents can be bound to as kind "external" (agent.bind_mind), run as its own process.
      const std::string spec = argv[++i];
      const size_t eq = spec.find('=');
      const std::string name = spec.substr(0, eq == std::string::npos ? 0 : eq);
      bool name_ok = !name.empty() && name.size() <= 32;
      for (unsigned char c : name) if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_')) name_ok = false;
      if (!name_ok || eq + 1 >= spec.size()) {
        std::cerr << "--external-mind wants name=command (a name of letters, digits and . - _), got: " << spec.substr(0, 40) << "\n";
        return 1;
      }
      if (!external_minds.emplace(name, spec.substr(eq + 1)).second) {
        std::cerr << "--external-mind " << name << " given twice\n";
        return 1;
      }
    }
  }

  // Never a bare argv value -- an argument ends up in the process list and
  // shell history. File only, same discipline as the qwen endpoint credential.
  std::string auth_secret;
  if (!auth_secret_file.empty()) {
    std::ifstream f(auth_secret_file);
    if (!f) {
      std::cerr << "Failed to open --auth-secret-file " << auth_secret_file << "\n";
      return 1;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    auth_secret = ss.str();
    while (!auth_secret.empty() && (auth_secret.back() == '\n' || auth_secret.back() == '\r')) auth_secret.pop_back();
  }

  if (!loopback_only && auth_secret.empty()) {
    std::cerr << "Refusing --listen-all without --auth-secret-file: every interface with no auth would let anyone on the network drive agents.\n";
    return 2;
  }

  std::cout << "========================================================================\n";
  std::cout << "  AIOS Agent Control Service (Phase 1: skeleton, no wire protocol yet)  \n";
  std::cout << "========================================================================\n";
  std::cout << "  Root Dir       : " << root_dir << "\n";
  if (!external_minds.empty()) {
    std::string names;
    for (const auto& m : external_minds) names += (names.empty() ? "" : ", ") + m.first;
    std::cout << "  External minds : " << names << "\n";
  }
  std::cout << "------------------------------------------------------------------------\n";

  KernelConfig kcfg;
  kcfg.root = root_dir;
  auto k_res = Kernel::boot(kcfg);
  if (!k_res.ok()) {
    std::cerr << "Failed to boot AIOS Kernel: " << k_res.error().message << "\n";
    return 1;
  }
  Kernel& kernel = **k_res;

  auto svc_res = kernel.resident("agent-control", agent_control_service_authority(), /*self_improve=*/false);
  if (!svc_res.ok()) {
    std::cerr << "Failed to create agent-control resident: " << svc_res.error().message << "\n";
    return 1;
  }
  EntityID svc_id = *svc_res;
  Surface svc = kernel.as(svc_id);

  std::cout << "  [ONLINE] agent-control resident: " << svc_id.str() << "\n";
  std::cout << "  Authority held:\n";
  for (const auto& cap : svc.authority()) {
    std::cout << "    " << cap.resource.str() << " : " << rights_str(cap.rights)
               << (cap.scope_limit ? " (scope " + std::to_string(cap.scope_limit) + ")" : "") << "\n";
  }
  std::cout << "\n";

  channel::AgentControlOptions opts;
  opts.port = port;
  opts.loopback_only = loopback_only;
  opts.auth_secret = auth_secret;
  opts.external_minds = std::move(external_minds);

  std::atomic<bool> wire_stop{false};
  hal::Thread wire_thread([&kernel, &svc, &opts, &wire_stop]() {
    auto r = channel::agent_control_serve(kernel, svc, opts,
        [](const std::string& line) { std::cout << "[wire] " << line << "\n"; },
        &wire_stop);
    if (!r.ok()) {
      std::cerr << "agent_control_serve exited with error: " << r.error().str() << "\n";
    }
  });

  while (!g_shutdown.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  std::cout << "\nShutting down Agent Control Service...\n";
  wire_stop.store(true);
  wire_thread.join();
  std::cout << "Agent Control Service safely stopped.\n";
  return 0;
}
