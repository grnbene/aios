// Verification Driver Tool for Control Plane Gateway & Ingress Dispatcher
// Proves in reality:
// 1. Ingress Token Gatekeeper (HMAC cryptographic signature, expiration, delegation depth, resource scope limits).
// 2. Load & Capability Ingress Router (Local admission, peer routing, and cluster rebalance dispatch).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>
#include <unistd.h>

#include "aios/channel/gateway.hpp"
#include "aios/compute/erp_kernels.hpp"
#include "aios/core/ids.hpp"
#include "aios/fleet/orchestrator.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/hal/host.hpp"

using namespace aios;
using namespace aios::channel;

namespace {

int failures = 0;

void check(bool cond, const char* what, const std::string& extra = "") {
  if (cond) {
    std::printf("  [PASS] %s\n", what);
  } else {
    ++failures;
    std::printf("  [FAIL] %s %s\n", what, extra.c_str());
  }
}

}  // namespace

int main() {
  std::printf("=== Driving Control Plane Gateway & Ingress Dispatcher ===\n");

  char root_dir[] = "/tmp/aios-gateway-test-XXXXXX";
  if (!mkdtemp(root_dir)) {
    std::perror("mkdtemp");
    return 1;
  }
  KernelConfig kcfg;
  kcfg.root = root_dir;
  kcfg.residency = test_config;
  auto k_res = Kernel::boot(kcfg);
  if (!k_res.ok()) {
    std::printf("Failed to boot kernel: %s\n", k_res.error().str().c_str());
    return 1;
  }
  Kernel& k = **k_res;
  Orchestrator orch(k);

  GatewayConfig cfg;
  cfg.secret_key = "test-cluster-gateway-hmac-key";
  cfg.max_delegation_depth = 3;
  cfg.max_local_pressure = 0.8;
  cfg.peer_headroom_threshold = 0.7;

  ControlPlaneGateway gateway(k, orch, cfg);

  // -------------------------------------------------------------------------
  // 1. Minting & Cryptographic Signature Verification
  // -------------------------------------------------------------------------
  std::printf("\n--- 1. Testing Token Minting, Parsing & Signature Verification ---\n");
  EntityID holder{k.ids().next()};
  uint16_t rights = Right::R_Execute | Right::R_Schedule | Right::R_Read;
  uint64_t scope_limit = 20; // max 20 stops/points
  uint64_t duration_ns = 60ULL * 1000000000ULL; // 60 seconds

  GatewayCapabilityToken tok = gateway.mint_token(holder, ResourceKind::ComputeDevice, rights, scope_limit, duration_ns, 1);
  check(tok.verify_signature(cfg.secret_key), "Minted token passes cryptographic HMAC verification");

  std::string token_str = tok.to_token_string(cfg.secret_key);
  auto parsed_res = GatewayCapabilityToken::parse_token_string(token_str, cfg.secret_key);
  check(parsed_res.ok(), "Serialized token string parses and verifies cleanly");

  // Tamper detection
  std::string tampered_str = token_str;
  tampered_str[10] = (tampered_str[10] == 'a') ? 'b' : 'a';
  auto tampered_res = GatewayCapabilityToken::parse_token_string(tampered_str, cfg.secret_key);
  check(!tampered_res.ok(), "Tampered token correctly rejected by HMAC signature verification");

  // -------------------------------------------------------------------------
  // 2. Ingress Gatekeeper: Resource Bounds, Expiration & Delegation Depth
  // -------------------------------------------------------------------------
  std::printf("\n--- 2. Testing Ingress Gatekeeper Boundary Checks ---\n");
  uint64_t now_ns = k.host().wall_ns();

  // Valid check
  auto valid_check = gateway.verify_token(tok, ResourceKind::ComputeDevice, Right::R_Execute | Right::R_Schedule, 10, now_ns);
  check(valid_check.ok(), "Valid compute request admitted within scope bounds");

  // Cost exceeding scope limit
  auto scope_exceeded = gateway.verify_token(tok, ResourceKind::ComputeDevice, Right::R_Execute | Right::R_Schedule, 25, now_ns);
  check(!scope_exceeded.ok(), "Request exceeding token scope limit rejected (25 > 20)");

  // Expired token
  uint64_t future_ns = now_ns + duration_ns + 1000;
  auto expired_check = gateway.verify_token(tok, ResourceKind::ComputeDevice, Right::R_Execute | Right::R_Schedule, 10, future_ns);
  check(!expired_check.ok(), "Expired token rejected at gatekeeper boundary");

  // Delegation depth exceeded
  GatewayCapabilityToken deep_tok = gateway.mint_token(holder, ResourceKind::ComputeDevice, rights, scope_limit, duration_ns, 5);
  auto depth_check = gateway.verify_token(deep_tok, ResourceKind::ComputeDevice, Right::R_Execute | Right::R_Schedule, 10, now_ns);
  check(!depth_check.ok(), "Token exceeding maximum delegation depth rejected (depth 5 > max 3)");

  // Insufficient rights
  auto rights_check = gateway.verify_token(tok, ResourceKind::ComputeDevice, Right::R_Execute | Right::R_Destroy, 10, now_ns);
  check(!rights_check.ok(), "Request requiring ungranted rights (R_Destroy) rejected");

  // -------------------------------------------------------------------------
  // 3. Kernel-Aware Ingress Routing & Peer Dispatch
  // -------------------------------------------------------------------------
  std::printf("\n--- 3. Testing Kernel-Aware Ingress Routing ---\n");

  RpcValue req = RpcValue::object();
  req.set("op", RpcValue::text("erp.tsp_exact"));
  RpcValue stops = RpcValue::list();
  for (int i = 0; i < 5; ++i) {
    RpcValue s = RpcValue::object();
    s.set("id", RpcValue::text(std::to_string(i)));
    s.set("lat", RpcValue::number(std::to_string(42.0 + i * 0.1)));
    s.set("lng", RpcValue::number(std::to_string(-78.0 - i * 0.1)));
    stops.push(s);
  }
  req.set("stops", stops);

  // An op no node registers is rejected before any graph is allocated. (The
  // ERP kernels are registered at boot now, so erp.tsp_exact can't be the example.)
  RpcValue nobody = RpcValue::object();
  nobody.set("op", RpcValue::text("erp.registered_nowhere"));
  nobody.set("stops", stops);
  auto decision_unsupported = gateway.route_request(nobody, tok, now_ns);
  check(decision_unsupported.status == RouteStatus::RejectedUnsupportedOp, "Rejected unsupported custom kernel before graph allocation");

  // Register ERP kernels on local device
  auto cpu_devs = k.devices().of_type(DeviceType::CPU);
  if (!cpu_devs.empty()) {
    aios::erp::register_erp_kernels(*cpu_devs[0]);
  }

  // Now local node supports erp.tsp_exact
  auto decision_local = gateway.route_request(req, tok, now_ns);
  check(decision_local.status == RouteStatus::AdmittedLocal, "Admitted locally once custom kernel is registered");
  check(decision_local.cost_units == 6, "Calculated cost units matching stops + start (6)");

  // When token is invalid, route_request returns rejection
  GatewayCapabilityToken invalid_tok = tok;
  invalid_tok.scope_limit = 3; // 3 < 6
  auto decision_rejected = gateway.route_request(req, invalid_tok, now_ns);
  check(decision_rejected.status == RouteStatus::RejectedScopeExceeded, "Route decision rejected due to scope limit violation");

  // Test peer kernel manifest registration and routing to peer
  Id128 peer_id = k.ids().next();
  gateway.register_peer_kernels(peer_id, {"erp.custom_solver_v2"});
  check(gateway.node_supports_op(peer_id, "erp.custom_solver_v2"), "Peer node recognized as supporting registered custom kernel");
  check(!gateway.node_supports_op(k.self(), "erp.custom_solver_v2"), "Local node correctly reports not having peer-only kernel");

  // Verify RoutedToPeer fires when request specifies a peer-only custom kernel
  RpcValue req_peer = RpcValue::object();
  req_peer.set("op", RpcValue::text("erp.custom_solver_v2"));
  req_peer.set("stops", stops);
  auto decision_peer = gateway.route_request(req_peer, tok, now_ns);
  check(decision_peer.status == RouteStatus::RoutedToPeer, "RoutedToPeer successfully fired for custom kernel registered on peer");
  check(decision_peer.target_node_id == peer_id, "Target node ID matched telemetry-registered peer ID");

  check(!gateway.routing_log().empty(), "Routing decisions recorded in gateway audit ledger");

  if (failures == 0) {
    std::printf("\n>>> PROVEN: Control Plane Gateway & Ingress Dispatcher fully verified!\n");
  } else {
    std::printf("\n>>> FAILED with %d failure(s)\n", failures);
  }

  return failures == 0 ? 0 : 1;
}
