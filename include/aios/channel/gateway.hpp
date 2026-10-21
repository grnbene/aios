#pragma once
// The Control Plane Gateway & Ingress Dispatcher.
// Sits at the front door between external client/edge requests and internal
// compute_serve worker nodes across the cluster.
//
// Capabilities are the gatekeeper for every dispatched graph:
// 1. Ingress Token Gatekeeper: Verifies tokens (HMAC/ECDSA signature, expiration,
//    resource kind, required rights mask, cost/scope bounds, and delegation depth).
// 2. Load & Capability Ingress Router: Surveys cluster peers via Orchestrator
//    telemetry, inspects pressure and headroom, checks registered custom kernels,
//    and routes incoming compute jobs to the optimal node.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include "aios/core/ids.hpp"
#include "aios/core/result.hpp"
#include "aios/capability/capability.hpp"
#include "aios/channel/rpc_value.hpp"
#include "aios/fleet/orchestrator.hpp"

namespace aios {
class Kernel;
}

namespace aios::channel {

// Structured Capability Token presented with an ingress request
struct GatewayCapabilityToken {
  CapabilityID capability_id;
  EntityID holder;
  ResourceKind resource_kind = ResourceKind::ComputeDevice;
  Id128 resource_id;         // null for all devices/resources of this kind
  uint16_t rights = 0;       // e.g. Right::R_Execute | Right::R_Schedule
  uint64_t scope_limit = 0;  // 0 = unbounded; otherwise max cost/stops/units
  uint64_t issued_ns = 0;
  uint64_t expires_ns = 0;   // 0 = never
  uint32_t delegation_depth = 0;
  std::string secret_salt;   // issuer salt
  std::string signature_hex; // HMAC-SHA256 signature

  std::string compute_signature(const std::string& secret_key) const;
  bool verify_signature(const std::string& secret_key) const;
  std::string to_token_string(const std::string& secret_key) const;
  static Result<GatewayCapabilityToken> parse_token_string(const std::string& token_str, const std::string& secret_key);
};

enum class RouteStatus : uint8_t {
  AdmittedLocal = 0,
  RoutedToPeer = 1,
  RebalancedAndMigrated = 2,
  RejectedUnauthorized = 3,
  RejectedExpired = 4,
  RejectedScopeExceeded = 5,
  RejectedOversubscribed = 6,
  RejectedUnsupportedOp = 7
};

const char* route_status_name(RouteStatus s);

struct GatewayRouteDecision {
  RouteStatus status = RouteStatus::AdmittedLocal;
  std::string op;
  Id128 target_node_id;
  std::string target_address;
  double local_pressure = 0.0;
  double target_pressure = 0.0;
  uint64_t cost_units = 0;
  std::string reason;
  uint64_t timestamp_ns = 0;
};

struct GatewayConfig {
  // Signs and checks capability tokens. Empty: resolve_secret() picks one.
  std::string secret_key;
  uint16_t port = 7310;
  bool require_token = true;
  uint32_t max_delegation_depth = 5;
  double max_local_pressure = 1.0;
  double peer_headroom_threshold = 0.75;
  bool use_tls = false;
  std::string cert_file;
  std::string key_file;
  bool require_custom_secret = false;
  // The one browser origin allowed to read answers (CORS). Empty: none -- no
  // Access-Control-Allow-Origin header is sent. "*" is refused by serve().
  std::string cors_origin;
};

class ControlPlaneGateway {
 public:
  ControlPlaneGateway(Kernel& k, Orchestrator& orch, GatewayConfig cfg = {});

  // Gatekeeper: Verifies token integrity, validity, resource rights, and scope
  Result<void> verify_token(const GatewayCapabilityToken& token, ResourceKind required_res, uint16_t required_rights, uint64_t cost_units, uint64_t now_ns) const;

  // Ingress Router: Makes placement & routing decision for incoming compute graph request
  GatewayRouteDecision route_request(const RpcValue& req, const GatewayCapabilityToken& token, uint64_t now_ns);

  // Mint a valid gateway capability token (Authority path)
  GatewayCapabilityToken mint_token(const EntityID& holder, ResourceKind kind, uint16_t rights, uint64_t scope_limit = 0, uint64_t duration_ns = 0, uint32_t delegation_depth = 0) const;

  // Audit log of recent gateway routing decisions
  const std::vector<GatewayRouteDecision>& routing_log() const { return log_; }

  // Peer & Local Kernel Manifest
  void register_peer_kernels(const Id128& peer_id, const std::vector<std::string>& kernels);
  std::vector<std::string> peer_kernels(const Id128& peer_id) const;
  std::vector<std::string> local_kernels() const;
  bool node_supports_op(const Id128& node_id, const std::string& op) const;

  // Settles the signing secret, the one rule serve() and the CLI's token/route
  // share: cfg.secret_key when set, else AIOS_GATEWAY_SECRET, else this root's
  // own key in <root>/gateway/secret, made the first time (32 random bytes, hex,
  // in an owner-only directory). A no-op once settled.
  Result<void> resolve_secret();
  // True when the secret is this root's own key rather than one the operator gave.
  bool secret_from_root() const { return secret_from_root_; }
  static std::string root_secret_path(const Kernel& k);

  // Run the HTTP/RPC socket listener for the Gateway on port (defaults to cfg_.port, e.g. 7310)
  Result<void> serve(uint16_t port = 0, bool loopback_only = true, std::atomic<bool>* stop = nullptr, std::function<void(const std::string&)> log = nullptr);

  const GatewayConfig& config() const { return cfg_; }
  void set_config(const GatewayConfig& cfg) { cfg_ = cfg; }

 private:
  Kernel& k_;
  Orchestrator& orch_;
  GatewayConfig cfg_;
  std::vector<GatewayRouteDecision> log_;
  std::unordered_map<Id128, std::vector<std::string>, Id128Hash> peer_kernels_;
  bool secret_from_root_ = false;
};

}  // namespace aios::channel
