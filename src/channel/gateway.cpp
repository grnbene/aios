#include "aios/hal/host.hpp"
#include "aios/channel/gateway.hpp"
#include <algorithm>
#include "aios/core/strbuf.hpp"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include "aios/core/bytes.hpp"
#include "aios/crypto/hmac_sha256.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/hal/net.hpp"
#include "aios/hal/sync.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/crypto/x509.hpp"
#include "aios/net/tls.hpp"
#include "aios/net/tls_conn.hpp"
#include "aios/storage/aql.hpp"

namespace aios::channel {

namespace {

std::string build_payload_str(const GatewayCapabilityToken& t) {
  std::string p;
  p.reserve(256);
  p += t.capability_id.str();
  p += ":";
  p += t.holder.str();
  p += ":";
  p += std::to_string(static_cast<uint32_t>(t.resource_kind));
  p += ":";
  p += t.resource_id.str();
  p += ":";
  p += std::to_string(t.rights);
  p += ":";
  p += std::to_string(t.scope_limit);
  p += ":";
  p += std::to_string(t.issued_ns);
  p += ":";
  p += std::to_string(t.expires_ns);
  p += ":";
  p += std::to_string(t.delegation_depth);
  p += ":";
  p += t.secret_salt;
  return p;
}

std::vector<std::string> split_str(const std::string& s, char delim) {
  return aios::split_lines(s, delim);
}

}  // namespace

std::string GatewayCapabilityToken::compute_signature(const std::string& secret_key) const {
  std::string payload = build_payload_str(*this);
  auto hmac = crypto::hmac_sha256(span_of(secret_key), span_of(payload));
  return crypto::bytes_to_hex(ByteSpan(hmac.data(), hmac.size()));
}

bool GatewayCapabilityToken::verify_signature(const std::string& secret_key) const {
  if (signature_hex.empty()) return false;
  std::string expected = compute_signature(secret_key);
  return expected == signature_hex;
}

std::string GatewayCapabilityToken::to_token_string(const std::string& secret_key) const {
  std::string sig = signature_hex.empty() ? compute_signature(secret_key) : signature_hex;
  std::string out;
  out += capability_id.str() + ".";
  out += holder.str() + ".";
  out += std::to_string(static_cast<uint32_t>(resource_kind)) + ".";
  out += resource_id.str() + ".";
  out += std::to_string(rights) + ".";
  out += std::to_string(scope_limit) + ".";
  out += std::to_string(issued_ns) + ".";
  out += std::to_string(expires_ns) + ".";
  out += std::to_string(delegation_depth) + ".";
  out += (secret_salt.empty() ? "0" : secret_salt) + ".";
  out += sig;
  return out;
}

Result<GatewayCapabilityToken> GatewayCapabilityToken::parse_token_string(const std::string& token_str, const std::string& secret_key) {
  auto parts = split_str(token_str, '.');
  if (parts.size() != 11) {
    return Result<GatewayCapabilityToken>(Status::Invalid, "malformed gateway capability token format (expected 11 fields)");
  }

  GatewayCapabilityToken tok;
  Id128 cap_raw, holder_raw, res_raw;
  if (!Id128::parse(parts[0], cap_raw)) return Result<GatewayCapabilityToken>(Status::Invalid, "invalid capability_id in token");
  if (!Id128::parse(parts[1], holder_raw)) return Result<GatewayCapabilityToken>(Status::Invalid, "invalid holder in token");
  if (!Id128::parse(parts[3], res_raw)) return Result<GatewayCapabilityToken>(Status::Invalid, "invalid resource_id in token");

  tok.capability_id = CapabilityID{cap_raw};
  tok.holder = EntityID{holder_raw};
  tok.resource_kind = static_cast<ResourceKind>(aios::to_u64(parts[2].c_str()));
  tok.resource_id = res_raw;
  tok.rights = static_cast<uint16_t>(aios::to_u64(parts[4].c_str()));
  tok.scope_limit = aios::to_u64(parts[5].c_str());
  tok.issued_ns = aios::to_u64(parts[6].c_str());
  tok.expires_ns = aios::to_u64(parts[7].c_str());
  tok.delegation_depth = static_cast<uint32_t>(aios::to_u64(parts[8].c_str()));
  tok.secret_salt = parts[9];
  tok.signature_hex = parts[10];

  if (!tok.verify_signature(secret_key)) {
    return Result<GatewayCapabilityToken>(Status::Denied, "invalid cryptographic signature on gateway capability token");
  }

  return tok;
}

const char* route_status_name(RouteStatus s) {
  switch (s) {
    case RouteStatus::AdmittedLocal: return "AdmittedLocal";
    case RouteStatus::RoutedToPeer: return "RoutedToPeer";
    case RouteStatus::RebalancedAndMigrated: return "RebalancedAndMigrated";
    case RouteStatus::RejectedUnauthorized: return "RejectedUnauthorized";
    case RouteStatus::RejectedExpired: return "RejectedExpired";
    case RouteStatus::RejectedScopeExceeded: return "RejectedScopeExceeded";
    case RouteStatus::RejectedOversubscribed: return "RejectedOversubscribed";
    case RouteStatus::RejectedUnsupportedOp: return "RejectedUnsupportedOp";
    default: return "Unknown";
  }
}

ControlPlaneGateway::ControlPlaneGateway(Kernel& k, Orchestrator& orch, GatewayConfig cfg)
    : k_(k), orch_(orch), cfg_(std::move(cfg)) {}

void ControlPlaneGateway::register_peer_kernels(const Id128& peer_id, const std::vector<std::string>& kernels) {
  peer_kernels_[peer_id] = kernels;
}

std::vector<std::string> ControlPlaneGateway::peer_kernels(const Id128& peer_id) const {
  auto it = peer_kernels_.find(peer_id);
  return it != peer_kernels_.end() ? it->second : std::vector<std::string>{};
}

std::vector<std::string> ControlPlaneGateway::local_kernels() const {
  std::vector<std::string> ops;
  for (auto* dev : k_.devices().all()) {
    Operation op_tsp;
    op_tsp.code = OpCode::Custom;
    op_tsp.custom_kernel = fnv1a("erp.tsp_exact");
    if (dev->supports(op_tsp)) ops.push_back("erp.tsp_exact");

    Operation op_route;
    op_route.code = OpCode::Custom;
    op_route.custom_kernel = fnv1a("erp.route_improve");
    if (dev->supports(op_route)) ops.push_back("erp.route_improve");

    Operation op_pool;
    op_pool.code = OpCode::Custom;
    op_pool.custom_kernel = fnv1a("erp.pool_greedy");
    if (dev->supports(op_pool)) ops.push_back("erp.pool_greedy");
  }
  return ops;
}

bool ControlPlaneGateway::node_supports_op(const Id128& node_id, const std::string& op) const {
  if (op.empty() || op == "nop" || op == "unknown") return true;
  std::string norm = op;
  if (norm == "tsp_exact") norm = "erp.tsp_exact";
  if (norm == "route_improve") norm = "erp.route_improve";
  if (norm == "pool_greedy") norm = "erp.pool_greedy";

  if (node_id == k_.self() || node_id == Id128{}) {
    Operation o;
    o.code = OpCode::Custom;
    o.custom_kernel = fnv1a(norm);
    for (auto* dev : k_.devices().all()) {
      if (dev->supports(o)) return true;
    }
    return false;
  }
  auto it = peer_kernels_.find(node_id);
  if (it != peer_kernels_.end()) {
    for (const auto& k : it->second) {
      if (k == norm || k == op) return true;
    }
  }
  return false;
}

Result<void> ControlPlaneGateway::verify_token(const GatewayCapabilityToken& token, ResourceKind required_res, uint16_t required_rights, uint64_t cost_units, uint64_t now_ns) const {
  if (cfg_.require_token) {
    if (!token.verify_signature(cfg_.secret_key)) {
      return Result<void>(Status::Denied, "capability token failed HMAC cryptographic verification");
    }
  }

  if (token.expires_ns > 0 && now_ns > token.expires_ns) {
    return Result<void>(Status::Expired, "capability token expired (expired_ns=" + std::to_string(token.expires_ns) + ", now_ns=" + std::to_string(now_ns) + ")");
  }

  if (token.delegation_depth > cfg_.max_delegation_depth) {
    return Result<void>(Status::Denied, "token delegation depth exceeded maximum allowable threshold (" + std::to_string(token.delegation_depth) + " > " + std::to_string(cfg_.max_delegation_depth) + ")");
  }

  if (token.resource_kind != required_res) {
    return Result<void>(Status::Invalid, "token resource kind mismatch (required " + std::string(resource_kind_name(required_res)) + ", got " + std::string(resource_kind_name(token.resource_kind)) + ")");
  }

  if ((token.rights & required_rights) != required_rights) {
    return Result<void>(Status::Denied, "insufficient capability rights (required " + rights_str(required_rights) + ", held " + rights_str(token.rights) + ")");
  }

  if (token.scope_limit > 0 && cost_units > token.scope_limit) {
    return Result<void>(Status::Exhausted, "resource cost units exceed token scope limit (" + std::to_string(cost_units) + " > " + std::to_string(token.scope_limit) + ")");
  }

  return Result<void>();
}

GatewayCapabilityToken ControlPlaneGateway::mint_token(const EntityID& holder, ResourceKind kind, uint16_t rights, uint64_t scope_limit, uint64_t duration_ns, uint32_t delegation_depth) const {
  GatewayCapabilityToken tok;
  tok.capability_id = CapabilityID{k_.ids().next()};
  tok.holder = holder;
  tok.resource_kind = kind;
  tok.resource_id = Id128{};
  tok.rights = rights;
  tok.scope_limit = scope_limit;
  tok.issued_ns = k_.host().wall_ns();
  tok.expires_ns = duration_ns > 0 ? (tok.issued_ns + duration_ns) : 0;
  tok.delegation_depth = delegation_depth;
  tok.secret_salt = std::to_string(k_.host().wall_ns() ^ 0xA5A5A5A5);
  tok.signature_hex = tok.compute_signature(cfg_.secret_key);
  return tok;
}

GatewayRouteDecision ControlPlaneGateway::route_request(const RpcValue& req, const GatewayCapabilityToken& token, uint64_t now_ns) {
  GatewayRouteDecision d;
  d.timestamp_ns = now_ns;
  d.op = req.has("op") ? req["op"].as_text() : "unknown";

  // Compute cost units
  uint64_t cost_units = 1;
  if (d.op == "tsp_exact" || d.op == "erp.tsp_exact") {
    if (req.has("stops") && req["stops"].kind() == RpcValue::Kind::List) {
      cost_units = req["stops"].size() + 1;
    }
  } else if (d.op == "route_improve") {
    if (req.has("stops") && req["stops"].kind() == RpcValue::Kind::List) {
      cost_units = req["stops"].size();
    }
  }
  d.cost_units = cost_units;

  // 1. Ingress Gatekeeper: Verify capability token
  uint16_t required_rights = Right::R_Execute | Right::R_Schedule;
  auto auth_check = verify_token(token, ResourceKind::ComputeDevice, required_rights, cost_units, now_ns);
  if (!auth_check.ok()) {
    if (token.expires_ns > 0 && now_ns > token.expires_ns) {
      d.status = RouteStatus::RejectedExpired;
    } else if (token.scope_limit > 0 && cost_units > token.scope_limit) {
      d.status = RouteStatus::RejectedScopeExceeded;
    } else {
      d.status = RouteStatus::RejectedUnauthorized;
    }
    d.reason = auth_check.error().message;
    log_.push_back(d);
    return d;
  }

  // 2. Kernel-Aware & Load Routing
  NodeTelemetry local_tele = orch_.local();
  d.local_pressure = local_tele.pressure();

  bool local_has_op = node_supports_op(k_.self(), d.op);

  auto peers = orch_.survey();
  std::vector<size_t> candidate_peer_indices;
  for (size_t i = 0; i < peers.size(); ++i) {
    if (!peers[i].reachable) continue;
    if (!peers[i].tele.custom_kernels.empty()) {
      register_peer_kernels(peers[i].tele.kernel, peers[i].tele.custom_kernels);
    }
    // Only a peer that reports the op among its kernels can take it: a peer
    // that never said it runs `op` is not a candidate just because it is up.
    if (node_supports_op(peers[i].tele.kernel, d.op)) {
      candidate_peer_indices.push_back(i);
    }
  }
  for (const auto& [pid, kernels] : peer_kernels_) {
    if (pid == k_.self()) continue;
    bool surveyed = false;
    for (size_t idx : candidate_peer_indices) {
      if (peers[idx].tele.kernel == pid) { surveyed = true; break; }
    }
    if (!surveyed && node_supports_op(pid, d.op)) {
      PeerView pv;
      pv.reachable = true;
      pv.address = "peer:" + pid.short_str();
      pv.tele.kernel = pid;
      pv.tele.movable_entities = 0;
      pv.tele.slots = 4;
      peers.push_back(pv);
      candidate_peer_indices.push_back(peers.size() - 1);
    }
  }

  // If no node in the fleet runs this op at all, say so -- that is
  // "unsupported", not "saturated".
  if (!local_has_op && candidate_peer_indices.empty()) {
    d.status = RouteStatus::RejectedUnsupportedOp;
    d.reason = "no node in the fleet has op '" + d.op + "' registered";
    log_.push_back(d);
    return d;
  }

  // If local supports op and has headroom, admit locally
  if (local_has_op && local_tele.pressure() < cfg_.max_local_pressure) {
    d.status = RouteStatus::AdmittedLocal;
    d.target_node_id = local_tele.kernel;
    d.target_address = "local";
    d.target_pressure = local_tele.pressure();
    d.reason = "Local kernel has sufficient headroom (pressure=" + std::to_string(local_tele.pressure()) + ")";
    log_.push_back(d);
    return d;
  }

  // If local is at capacity or lacks the custom op, search candidate peers
  int best_peer_idx = -1;
  double lowest_peer_pressure = 999.0;

  for (size_t idx : candidate_peer_indices) {
    const auto& p = peers[idx];
    if (p.tele.pressure() < lowest_peer_pressure && p.tele.pressure() < cfg_.peer_headroom_threshold) {
      lowest_peer_pressure = p.tele.pressure();
      best_peer_idx = (int)idx;
    }
  }

  if (best_peer_idx >= 0) {
    const auto& best_peer = peers[best_peer_idx];
    d.status = RouteStatus::RoutedToPeer;
    d.target_node_id = best_peer.tele.kernel;
    d.target_address = best_peer.address;
    d.target_pressure = lowest_peer_pressure;
    d.reason = "Rerouted to capable peer " + best_peer.address + " (pressure=" + std::to_string(lowest_peer_pressure) + ")";
    log_.push_back(d);
    return d;
  }

  // If local supports op and oversubscribed, invoke rebalance
  if (local_has_op) {
    auto rebalance_decisions = orch_.rebalance(true);
    if (!rebalance_decisions.empty()) {
      d.status = RouteStatus::RebalancedAndMigrated;
      d.target_node_id = local_tele.kernel;
      d.target_address = "local";
      d.reason = "Triggered autonomous fleet rebalance to relieve cluster pressure: moved job " + rebalance_decisions.front().entity.short_str();
      log_.push_back(d);
      return d;
    }
  }

  d.status = RouteStatus::RejectedOversubscribed;
  d.reason = "Fleet nodes capable of running '" + d.op + "' are fully saturated";
  log_.push_back(d);
  return d;
}

namespace {

bool read_gateway_request(hal::Connection& c, std::string& out, int timeout_ms) {
  out.clear();
  uint8_t ch = 0;
  if (!c.recv_all(MutableByteSpan(&ch, 1), timeout_ms)) return false;
  out.push_back((char)ch);

  // If binary length header (not ASCII printable character)
  if (ch < 0x20 || ch > 0x7E) {
    uint8_t hdr[3];
    if (c.recv_all(MutableByteSpan(hdr, 3), 100)) {
      uint32_t len = ((uint32_t)ch << 24) | ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | (uint32_t)hdr[2];
      if (len > 0 && len <= (4u << 20)) {
        out.resize(len);
        return c.recv_all(MutableByteSpan(reinterpret_cast<uint8_t*>(out.data()), len), timeout_ms);
      }
    }
  }

  // Read HTTP headers or raw JSON
  while (out.size() < 65536) {
    if (out.find("\r\n\r\n") != std::string::npos || out.find("\n\n") != std::string::npos) {
      break;
    }
    if (out.front() == '{') {
      int depth = 0;
      for (char x : out) { if (x == '{') ++depth; else if (x == '}') --depth; }
      if (depth == 0 && out.size() > 1) return true;
    }
    if (!c.recv_all(MutableByteSpan(&ch, 1), 200)) {
      if (!out.empty()) break;
      return false;
    }
    out.push_back((char)ch);
  }

  // If HTTP request, check Content-Length
  size_t cl_pos = out.find("Content-Length:");
  if (cl_pos == std::string::npos) cl_pos = out.find("content-length:");
  if (cl_pos != std::string::npos) {
    size_t line_end = out.find("\r\n", cl_pos);
    if (line_end != std::string::npos) {
      std::string cl_str = out.substr(cl_pos + 15, line_end - (cl_pos + 15));
      size_t content_len = aios::to_u64(cl_str.c_str());
      size_t header_end = out.find("\r\n\r\n");
      size_t body_start = (header_end != std::string::npos) ? (header_end + 4) : out.size();
      size_t body_have = out.size() - body_start;
      if (body_have < content_len) {
        size_t need_bytes = content_len - body_have;
        size_t cur_sz = out.size();
        out.resize(cur_sz + need_bytes);
        if (!c.recv_all(MutableByteSpan(reinterpret_cast<uint8_t*>(out.data() + cur_sz), need_bytes), timeout_ms)) {
          return false;
        }
      }
    }
  }
  return true;
}

std::string json_esc(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char ch : s) {
    switch (ch) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if ((unsigned char)ch < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", (unsigned)(unsigned char)ch);
          o += buf;
        } else {
          o += ch;
        }
    }
  }
  return o;
}

bool ieq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
  return true;
}

// A request header's value, looked up case-insensitively in the header block
// only -- never in the body, where a JSON string could otherwise pose as one.
std::string header_value(const std::string& raw, const std::string& name) {
  size_t end = raw.find("\r\n\r\n");
  if (end == std::string::npos) end = raw.find("\n\n");
  if (end == std::string::npos) end = raw.size();
  size_t pos = raw.find('\n');   // skip the request line
  while (pos != std::string::npos && pos < end) {
    size_t ls = pos + 1;
    size_t le = raw.find('\n', ls);
    if (le == std::string::npos || le > end) le = end;
    std::string line = raw.substr(ls, le - ls);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t colon = line.find(':');
    if (colon != std::string::npos && ieq(line.substr(0, colon), name)) {
      std::string v = line.substr(colon + 1);
      size_t a = v.find_first_not_of(" \t");
      size_t b = v.find_last_not_of(" \t");
      return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
    }
    if (le >= end) break;
    pos = le;
  }
  return {};
}

// Constant-time compare, so a wrong secret leaks nothing about how close it was.
bool ct_eq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char d = 0;
  for (size_t i = 0; i < a.size(); ++i) d |= (unsigned char)(a[i] ^ b[i]);
  return d == 0;
}

// `cors_origin` empty: no CORS headers at all, so a browser on another origin
// cannot read the answer. Set: that one origin, and only when the request came
// from it.
bool write_http_response(hal::Connection& c, int status_code, const std::string& content_type, const std::string& body,
                         const std::string& cors_origin = {}, const std::string& request_origin = {}) {
  const char* reason = "Bad Request";
  switch (status_code) {
    case 200: reason = "OK"; break;
    case 204: reason = "No Content"; break;
    case 401: reason = "Unauthorized"; break;
    case 403: reason = "Forbidden"; break;
    case 404: reason = "Not Found"; break;
    case 405: reason = "Method Not Allowed"; break;
    case 429: reason = "Too Many Requests"; break;
    default: break;
  }
  std::string res = "HTTP/1.1 " + std::to_string(status_code) + " " + reason + "\r\n";
  res += "Content-Type: " + content_type + "\r\n";
  res += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  if (!cors_origin.empty() && request_origin == cors_origin) {
    res += "Access-Control-Allow-Origin: " + cors_origin + "\r\n";
    res += "Vary: Origin\r\n";
    res += "Access-Control-Allow-Headers: Content-Type, Authorization, X-Capability-Token, X-Gateway-Secret\r\n";
    res += "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
  }
  res += "Connection: close\r\n\r\n";
  res += body;
  return c.send_all(span_of(res));
}

}  // namespace

std::string ControlPlaneGateway::root_secret_path(const Kernel& k) { return k.config().root + "/gateway/secret"; }

Result<void> ControlPlaneGateway::resolve_secret() {
  if (!cfg_.secret_key.empty()) return Ok();
  const std::string env_sec = hal::process_host().env("AIOS_GATEWAY_SECRET");
  if (!env_sec.empty()) {
    cfg_.secret_key = env_sec;
    return Ok();
  }
  // A fleet's gateways must accept each other's tokens, so they share one
  // secret the operator sets; a root's own key would quietly differ per machine.
  if (cfg_.require_custom_secret)
    return Result<void>(Status::Denied, "require_custom_secret=1: set secret= or AIOS_GATEWAY_SECRET (this root's own key is not used)");
  // There is no built-in secret: one printed in the source would let anyone
  // with the tree sign tokens. Each root keeps its own, made on first use.
  auto& h = k_.host();
  const std::string path = root_secret_path(k_);
  auto read_key = [&]() -> Result<void> {
    auto f = h.read_file(path);
    if (!f.ok()) return Result<void>(f.error().status, path + ": " + f.error().message);
    std::string s(reinterpret_cast<const char*>(f->data()), f->size());
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (s.size() != 64 || s.find_first_not_of("0123456789abcdef") != std::string::npos)
      return Result<void>(Status::Corrupt, path + ": not a gateway secret (64 hex characters); remove it to make a new one");
    cfg_.secret_key = s;
    secret_from_root_ = true;
    return Ok();
  };
  if (h.exists(path)) return read_key();
  // The directory is owner-only before the key is written into it, so the key
  // is never readable by another account, not even for a moment.
  const std::string dir = path.substr(0, path.rfind('/'));
  AIOS_TRY(h.make_dir(dir));
  AIOS_TRY(h.set_access(dir, hal::Host::FileAccess::Private));
  uint8_t raw[32];
  h.random(MutableByteSpan(raw, sizeof raw));
  const std::string hex = crypto::bytes_to_hex(ByteSpan(raw, sizeof raw)) + "\n";
  // Two gateways starting at once on a new root: one creates the key, the
  // other finds it and uses the same one.
  auto made = h.create_exclusive(path, span_of(hex));
  if (!made.ok() && made.error().status != Status::Conflict) return made;
  AIOS_TRY(h.set_access(path, hal::Host::FileAccess::Private));
  return read_key();
}

Result<void> ControlPlaneGateway::serve(uint16_t port, bool loopback_only, std::atomic<bool>* stop, std::function<void(const std::string&)> log) {
  AIOS_TRY(resolve_secret());
  if (cfg_.cors_origin == "*") {
    return Result<void>(Status::Invalid, "cors=* is refused: name the one origin allowed to read gateway answers (e.g. cors=https://ops.example.com)");
  }

  // tls=1 serves TLS or does not start: the identity is loaded and checked up
  // front, so a missing or unreadable cert/key is a startup error, never a
  // silent fall back to plaintext.
  std::vector<crypto::Certificate> tls_chain;
  crypto::PrivateKey tls_key;
  if (cfg_.use_tls) {
    if (cfg_.cert_file.empty() || cfg_.key_file.empty())
      return Result<void>(Status::Invalid, "tls=1 needs cert=PATH (PEM chain) and key=PATH (PEM private key)");
    auto cert_raw = k_.host().read_file(cfg_.cert_file);
    if (!cert_raw.ok()) return Result<void>(Status::NotFound, "tls cert " + cfg_.cert_file + ": " + cert_raw.error().message);
    auto chain = crypto::x509_parse_bundle(ByteSpan(cert_raw->data(), cert_raw->size()));
    if (!chain.ok() || chain->empty())
      return Result<void>(Status::Invalid, "tls cert " + cfg_.cert_file + ": no certificate could be parsed" + (chain.ok() ? std::string() : " (" + chain.error().message + ")"));
    auto key_raw = k_.host().read_file(cfg_.key_file);
    if (!key_raw.ok()) return Result<void>(Status::NotFound, "tls key " + cfg_.key_file + ": " + key_raw.error().message);
    auto key = crypto::x509_parse_private_key(ByteSpan(key_raw->data(), key_raw->size()));
    if (!key.ok() || !key->valid())
      return Result<void>(Status::Invalid, "tls key " + cfg_.key_file + ": not a usable private key" + (key.ok() ? std::string() : " (" + key.error().message + ")"));
    tls_chain = std::move(*chain);
    tls_key = std::move(*key);
  }

  uint16_t bind_port = port > 0 ? port : cfg_.port;
  auto listener = hal::net_listen(bind_port, loopback_only);
  if (!listener.ok()) return listener.error();

  if (log) {
    log(std::string("control plane gateway listening on ") + (loopback_only ? "127.0.0.1" : "0.0.0.0") + ":" +
        std::to_string((*listener)->port()) +
        (cfg_.use_tls ? " [HTTPS: TLS 1.3/1.2, cert " + tls_chain.front().subject_cn + "]" : std::string(" [plain HTTP/1.1 + wire]")) +
        (cfg_.cors_origin.empty() ? "" : " cors=" + cfg_.cors_origin));
  }

  while (!(stop && stop->load())) {
    auto conn = (*listener)->accept(200);
    if (!conn.ok()) {
      if (conn.error().status == Status::Busy) continue;
      if (log) log("gateway accept: " + conn.error().str());
      hal::sleep_ms(50);
      continue;
    }
    std::unique_ptr<hal::Connection> c(std::move(*conn));

    if (cfg_.use_tls) {
      std::string peer = c->peer();
      auto tls = std::make_unique<net::TlsServer>(k_.host(), tls_chain, tls_key);
      auto acc = tls->accept(std::move(c), 5000);
      if (!acc.ok()) {
        if (log) log("gateway tls handshake from " + peer + " failed: " + acc.error().str());
        continue;
      }
      c = std::make_unique<net::TlsServerConnection>(std::move(tls), peer);
    }

    std::string raw_req;
    if (!read_gateway_request(*c, raw_req, 5000)) {
      c->close();
      continue;
    }

    uint64_t now_ns = k_.host().wall_ns();

    // Check if HTTP request
    bool is_http = (raw_req.rfind("GET ", 0) == 0 || raw_req.rfind("POST ", 0) == 0 || raw_req.rfind("OPTIONS ", 0) == 0 || raw_req.rfind("HEAD ", 0) == 0);

    if (is_http) {
      // Parse method and path
      size_t first_sp = raw_req.find(' ');
      size_t second_sp = raw_req.find(' ', first_sp + 1);
      std::string method = raw_req.substr(0, first_sp);
      std::string path = (first_sp != std::string::npos && second_sp != std::string::npos) ? raw_req.substr(first_sp + 1, second_sp - (first_sp + 1)) : "/";
      std::string route = path.substr(0, path.find('?'));
      std::string req_body;
      size_t body_pos = raw_req.find("\r\n\r\n");
      if (body_pos != std::string::npos) req_body = raw_req.substr(body_pos + 4);
      else {
        body_pos = raw_req.find("\n\n");
        if (body_pos != std::string::npos) req_body = raw_req.substr(body_pos + 2);
      }
      const std::string origin = header_value(raw_req, "Origin");
      auto reply = [&](int code, const std::string& body) {
        write_http_response(*c, code, "application/json", body, cfg_.cors_origin, origin);
        c->close();
      };

      if (method == "OPTIONS") {
        write_http_response(*c, 204, "text/plain", "", cfg_.cors_origin, origin);
        c->close();
        continue;
      }

      // Who is asking. The operator proves itself with the gateway secret
      // (X-Gateway-Secret); a client with a capability token signed by it.
      const std::string presented_secret = header_value(raw_req, "X-Gateway-Secret");
      const bool has_secret = !presented_secret.empty() && ct_eq(presented_secret, cfg_.secret_key);
      auto parsed_body = rpc_parse(req_body);
      RpcValue req_rpc = (parsed_body.ok() && parsed_body->kind() == RpcValue::Kind::Object) ? *parsed_body : RpcValue::object();
      std::string tok_str;
      {
        std::string authz = header_value(raw_req, "Authorization");
        if (authz.size() > 7 && ieq(authz.substr(0, 7), "Bearer ")) tok_str = authz.substr(7);
      }
      if (tok_str.empty()) tok_str = header_value(raw_req, "X-Capability-Token");
      if (tok_str.empty() && req_rpc.has("token")) tok_str = req_rpc["token"].as_text();
      GatewayCapabilityToken tok;
      bool has_token = false;
      if (!tok_str.empty()) {
        auto parsed_tok = GatewayCapabilityToken::parse_token_string(tok_str, cfg_.secret_key);
        if (parsed_tok.ok()) {
          tok = *parsed_tok;
          has_token = !(tok.expires_ns > 0 && now_ns > tok.expires_ns);
        }
      }
      // Reads of machine state: a valid token or the secret. Minting and
      // anything that changes the machine: the secret only.
      auto deny_read = [&]() {
        if (has_secret || has_token) return false;
        reply(401, "{\"ok\":false,\"error\":\"unauthorized: send a capability token (Authorization: Bearer / X-Capability-Token) or the gateway secret (X-Gateway-Secret)\"}\n");
        return true;
      };
      auto deny_admin = [&]() {
        if (has_secret) return false;
        reply(has_token ? 403 : 401, "{\"ok\":false,\"error\":\"this endpoint needs the gateway secret (X-Gateway-Secret); a capability token is not enough\"}\n");
        return true;
      };

      if (method == "GET" && (route == "/" || route == "/status" || route == "/health")) {
        NodeTelemetry local_tele = orch_.local();
        std::string j = "{\"status\":\"ok\",\"gateway\":\"aios-control-plane\",\"local_pressure\":" + std::to_string(local_tele.pressure()) +
                        ",\"routing_events\":" + std::to_string(log_.size()) +
                        ",\"free_slots\":" + std::to_string(local_tele.free_slots()) +
                        ",\"headroom_threshold\":" + std::to_string(cfg_.peer_headroom_threshold) + "}\n";
        reply(200, j);
        continue;
      }

      if (method == "GET" && (route == "/fleet" || route == "/telemetry")) {
        if (deny_read()) continue;
        NodeTelemetry local_tele = orch_.local();
        auto peers = orch_.survey();
        std::string j;
        j.reserve(2048);
        j += "{\n  \"status\":\"ok\",\n  \"gateway\":\"aios-control-plane\",\n";
        j += "  \"pressure\":" + std::to_string(local_tele.pressure()) + ",\n";
        j += "  \"free_slots\":" + std::to_string(local_tele.free_slots()) + ",\n";
        j += "  \"active_nodes\":" + std::to_string(peers.size() + 1) + ",\n";
        j += "  \"routing_events\":" + std::to_string(log_.size()) + ",\n";

        // Build nodes array. Only this machine's arch is known here: peer
        // telemetry does not carry one, so peers have no "arch" field rather
        // than a copy of ours.
        j += "  \"nodes\": [\n";
        const char* node_arch =
#if defined(__aarch64__)
            "aarch64";
#else
            "x86_64";
#endif
        j += "    {\"id\":\"" + local_tele.kernel.short_str() + "\",\"host\":\"local\",\"arch\":\"" + node_arch + "\",\"pressure\":" + std::to_string(local_tele.pressure()) + ",\"queued\":" + std::to_string(local_tele.movable_entities) + ",\"free_slots\":" + std::to_string(local_tele.free_slots()) + "}";
        for (const auto& p : peers) {
          j += ",\n    {\"id\":\"" + p.tele.kernel.short_str() + "\",\"host\":\"" + json_esc(p.address) + "\",\"reachable\":" + (p.reachable ? "true" : "false") + ",\"pressure\":" + std::to_string(p.tele.pressure()) + ",\"queued\":" + std::to_string(p.tele.movable_entities) + ",\"free_slots\":" + std::to_string(p.tele.free_slots()) + "}";
        }
        j += "\n  ],\n";

        // Migrations: what the orchestrator actually recorded for each decision.
        j += "  \"migrations\": [\n";
        const auto& olog = orch_.log();
        bool first_m = true;
        for (const auto& dec : olog) {
          if (dec.kind == PlacementDecision::Hold) continue;
          if (!first_m) j += ",\n";
          first_m = false;
          j += "    {\"id\":\"" + dec.entity.short_str() + "\",\"kind\":\"" + dec.kind_name() + "\",\"from\":\"" + dec.from.short_str() + "\",\"to\":\"" + dec.to.short_str() +
               "\",\"from_pressure\":" + std::to_string(dec.from_pressure) + ",\"to_pressure\":" + std::to_string(dec.to_pressure) +
               ",\"status\":\"" + (dec.executed ? "COMPLETED" : "PLANNED") + "\",\"ns\":" + std::to_string(dec.ns) +
               ",\"reason\":\"" + json_esc(dec.reason) + "\"}";
        }
        j += "\n  ]\n}\n";

        reply(200, j);
        continue;
      }

      if (method == "GET" && route == "/checkpoints") {
        if (deny_read()) continue;
        // List stored objects/checkpoints in object store
        std::string j = "{\n  \"status\":\"ok\",\n  \"checkpoints\": [\n";
        auto list = k_.store().list();
        bool first = true;
        for (const auto& obj_id : list) {
          if (!first) j += ",\n";
          first = false;
          auto obj = k_.store().get(obj_id);
          std::string ts = obj.ok() ? std::to_string(obj->timestamp_ns) : "0";
          j += "    {\"id\":\"" + obj_id.short_str() + "\",\"timestamp_ns\":" + ts + "}";
        }
        j += "\n  ]\n}\n";
        reply(200, j);
        continue;
      }

      if (route == "/query" && (method == "GET" || method == "POST")) {
        if (deny_read()) continue;
        std::string qstr;
        if (method == "GET") {
          auto qpos = path.find("q=");
          if (qpos != std::string::npos) {
            qstr = path.substr(qpos + 2);
            auto amppos = qstr.find('&');
            if (amppos != std::string::npos) qstr = qstr.substr(0, amppos);
            // simple url decoding
            std::string decoded;
            for (size_t i = 0; i < qstr.size(); ++i) {
              if (qstr[i] == '+' || qstr[i] == '%') {
                if (qstr[i] == '+') decoded += ' ';
                else if (i + 2 < qstr.size()) {
                  decoded += (char)aios::to_u64(qstr.substr(i + 1, 2).c_str(), nullptr, 16);
                  i += 2;
                }
              } else decoded += qstr[i];
            }
            qstr = decoded;
          }
        } else {
          qstr = req_rpc.has("query") ? req_rpc["query"].as_text() : req_body;
        }
        if (qstr.empty()) qstr = "SELECT * FROM objects LIMIT 20";

        auto res = aios::storage::execute_aql(k_.store(), qstr, &k_.host(), k_.config().root);
        std::string j = "{\n  \"status\":\"ok\",\n  \"count\":" + std::to_string(res.objects.size()) + ",\n  \"objects\": [\n";
        bool first = true;
        for (const auto& obj : res.objects) {
          if (!first) j += ",\n";
          first = false;
          j += "    {\"id\":\"" + obj.id.str() + "\",\"timestamp_ns\":" + std::to_string(obj.timestamp_ns) + ",\"provenance\":\"" + obj.provenance.str() + "\"";
          if (!obj.metadata.empty()) {
            j += ",\"meta\":{";
            bool first_m = true;
            for (const auto& [mk, mv] : obj.metadata) {
              if (!first_m) j += ",";
              first_m = false;
              j += "\"" + json_esc(mk) + "\":\"" + json_esc(mv) + "\"";
            }
            j += "}";
          }
          j += "}";
        }
        j += "\n  ]\n}\n";
        reply(200, j);
        continue;
      }

      if (route == "/token" || route == "/mint_token") {
        if (method != "GET" && method != "POST") { reply(405, "{\"ok\":false,\"error\":\"GET or POST\"}\n"); continue; }
        if (deny_admin()) continue;
        uint64_t scope = req_rpc.has("scope") ? req_rpc["scope"].as_u64(100) : 100;
        uint64_t duration_s = req_rpc.has("duration_s") ? req_rpc["duration_s"].as_u64(3600) : 3600;
        EntityID holder = k_.self();
        GatewayCapabilityToken minted = mint_token(holder, ResourceKind::ComputeDevice, Right::R_Execute | Right::R_Schedule, scope, duration_s * 1000000000ULL, 0);
        std::string j = "{\"ok\":true,\"token\":\"" + minted.to_token_string(cfg_.secret_key) + "\"}\n";
        reply(200, j);
        continue;
      }

      if (method == "POST" && route == "/rebalance") {
        if (deny_admin()) continue;
        auto rebalances = orch_.rebalance(true);
        reply(200, "{\"ok\":true,\"rebalances\":" + std::to_string(rebalances.size()) + "}\n");
        continue;
      }

      if (method == "POST" && route == "/rewind") {
        if (deny_admin()) continue;
        std::string chk_path = req_rpc.has("path") ? req_rpc["path"].as_text() : "";
        auto rw_res = k_.rewind(chk_path, k_.self());
        if (rw_res.ok()) {
          reply(200, "{\"ok\":true,\"entity\":\"" + rw_res->short_str() + "\",\"status\":\"rewound\"}\n");
        } else {
          reply(400, "{\"ok\":false,\"error\":\"" + json_esc(rw_res.error().message) + "\"}\n");
        }
        continue;
      }

      if (method != "POST") {
        reply(404, "{\"ok\":false,\"error\":\"no such endpoint: " + json_esc(method + " " + route) + "\"}\n");
        continue;
      }

      // POST /route, POST /dispatch, POST / (any other POST): a routing
      // decision, gated by the capability token found above.
      GatewayRouteDecision d = route_request(req_rpc, tok, now_ns);

      int http_status = 200;
      if (d.status == RouteStatus::RejectedUnauthorized || d.status == RouteStatus::RejectedExpired || d.status == RouteStatus::RejectedScopeExceeded) {
        http_status = 403;
      } else if (d.status == RouteStatus::RejectedOversubscribed) {
        http_status = 429;
      } else if (d.status == RouteStatus::RejectedUnsupportedOp) {
        http_status = 400;
      }

      bool is_admitted = (d.status == RouteStatus::AdmittedLocal || d.status == RouteStatus::RoutedToPeer || d.status == RouteStatus::RebalancedAndMigrated);
      std::string j = "{\"ok\":" + std::string(is_admitted ? "true" : "false") +
                      ",\"status\":\"" + std::string(route_status_name(d.status)) + "\"" +
                      ",\"op\":\"" + json_esc(d.op) + "\"" +
                      ",\"target_node_id\":\"" + d.target_node_id.str() + "\"" +
                      ",\"target_address\":\"" + json_esc(d.target_address) + "\"" +
                      ",\"local_pressure\":" + std::to_string(d.local_pressure) +
                      ",\"target_pressure\":" + std::to_string(d.target_pressure) +
                      ",\"cost_units\":" + std::to_string(d.cost_units) +
                      ",\"reason\":\"" + json_esc(d.reason) + "\"}\n";

      reply(http_status, j);
      continue;
    }

    // Raw JSON / RPC framing path
    auto parsed_json = rpc_parse(raw_req);
    if (!parsed_json.ok() || parsed_json->kind() != RpcValue::Kind::Object) {
      c->close();
      continue;
    }
    const RpcValue& req_rpc = *parsed_json;
    std::string tok_str = req_rpc.has("token") ? req_rpc["token"].as_text() : "";
    GatewayCapabilityToken tok;
    if (!tok_str.empty()) {
      auto parsed_tok = GatewayCapabilityToken::parse_token_string(tok_str, cfg_.secret_key);
      if (parsed_tok.ok()) tok = *parsed_tok;
    }
    GatewayRouteDecision d = route_request(req_rpc, tok, now_ns);
    bool is_admitted = (d.status == RouteStatus::AdmittedLocal || d.status == RouteStatus::RoutedToPeer || d.status == RouteStatus::RebalancedAndMigrated);
    std::string ans = "{\"ok\":" + std::string(is_admitted ? "true" : "false") +
                      ",\"status\":\"" + std::string(route_status_name(d.status)) + "\"" +
                      ",\"op\":\"" + json_esc(d.op) + "\"" +
                      ",\"target_node_id\":\"" + d.target_node_id.str() + "\"" +
                      ",\"target_address\":\"" + json_esc(d.target_address) + "\"" +
                      ",\"reason\":\"" + json_esc(d.reason) + "\"}\n";
    c->send_all(span_of(ans));
    c->close();
  }

  (*listener)->close();
  return Ok();
}

}  // namespace aios::channel
