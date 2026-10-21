#include "aios/channel/peer_discovery.hpp"
#include <algorithm>
#include <chrono>
#include "aios/core/bytes.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/net.hpp"

namespace aios::channel {

namespace {

uint64_t current_time_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

uint8_t hex_char_to_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

uint16_t parse_u16(const std::string& s) {
  uint32_t val = 0;
  for (char c : s) {
    if (c >= '0' && c <= '9') {
      val = val * 10 + (c - '0');
    }
  }
  return static_cast<uint16_t>(val);
}

// This machine's LAN address, the one a beacon names: the first interface that
// is up, not loopback and not a container/VM bridge (docker*, br-*, veth*,
// virbr*), else any non-loopback one, else 127.0.0.1.
std::string detect_local_ip() {
  auto host = hal::make_host();
  if (!host) return "127.0.0.1";
  auto net = host->network();
  if (!net.ok()) return "127.0.0.1";
  std::string fallback;
  for (const auto& iface : *net) {
    if (!iface.up || iface.loopback || iface.address.empty() || iface.address == "0.0.0.0") continue;
    const bool bridge = iface.name.rfind("docker", 0) == 0 || iface.name.rfind("br-", 0) == 0 ||
                        iface.name.rfind("veth", 0) == 0 || iface.name.rfind("virbr", 0) == 0;
    if (!bridge) return iface.address;
    if (fallback.empty()) fallback = iface.address;
  }
  return fallback.empty() ? "127.0.0.1" : fallback;
}

std::string rstrip(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
  return s;
}

}  // namespace

NodeID hex_to_node_id(const std::string& hex) {
  NodeID id{};
  size_t len = hex.length();
  for (size_t i = 0; i < 20; ++i) {
    if (i * 2 < len) {
      uint8_t high = hex_char_to_val(hex[i * 2]);
      uint8_t low = (i * 2 + 1 < len) ? hex_char_to_val(hex[i * 2 + 1]) : 0;
      id[i] = (high << 4) | low;
    } else {
      id[i] = 0;
    }
  }
  return id;
}

std::string node_id_to_hex(const NodeID& id) {
  static const char hex_chars[] = "0123456789abcdef";
  std::string s;
  s.reserve(40);
  for (size_t i = 0; i < 20; ++i) {
    s.push_back(hex_chars[(id[i] >> 4) & 0x0F]);
    s.push_back(hex_chars[id[i] & 0x0F]);
  }
  return s;
}

NodeID xor_distance(const NodeID& a, const NodeID& b) {
  NodeID res{};
  for (size_t i = 0; i < 20; ++i) {
    res[i] = a[i] ^ b[i];
  }
  return res;
}

int common_prefix_length(const NodeID& distance) {
  int bits = 0;
  for (size_t i = 0; i < 20; ++i) {
    if (distance[i] == 0) {
      bits += 8;
    } else {
      uint8_t byte_val = distance[i];
      for (int b = 7; b >= 0; --b) {
        if ((byte_val & (1 << b)) == 0) {
          bits++;
        } else {
          break;
        }
      }
      break;
    }
  }
  return bits;
}

NodeID generate_node_id(const std::string& seed) {
  ByteSpan span(reinterpret_cast<const uint8_t*>(seed.data()), seed.size());
  auto hash = crypto::sha256(span);
  NodeID id{};
  std::copy(hash.begin(), hash.begin() + 20, id.begin());
  return id;
}

P2PMeshDiscovery::P2PMeshDiscovery()
    : P2PMeshDiscovery("aios-default-node-id-seed") {}

P2PMeshDiscovery::P2PMeshDiscovery(const std::string& node_id) {
  local_ip_ = detect_local_ip();
  if (node_id.length() == 40) {
    local_node_id_ = hex_to_node_id(node_id);
    local_node_id_str_ = node_id;
  } else {
    local_node_id_ = generate_node_id(node_id);
    local_node_id_str_ = node_id_to_hex(local_node_id_);
  }
}

// One announcement on the LAN broadcast, now. Active means one actually went
// out from this process; a failed send is the error, not a status line.
Result<void> P2PMeshDiscovery::start_mdns_beacon(uint16_t port) {
  mdns_port_ = port;
  auto sent = broadcast_beacon(kBeaconPort, port);
  if (!sent.ok()) return sent;
  mdns_active_ = true;
  return Ok();
}

Result<void> P2PMeshDiscovery::stop_mdns_beacon() {
  mdns_active_ = false;
  return Ok();
}

std::string P2PMeshDiscovery::generate_mdns_announcement(uint16_t port) const {
  return "AIOS_P2P_MDNS_v1:" + local_node_id_str_ + ":" + (local_ip_.empty() ? std::string("127.0.0.1") : local_ip_) + ":" +
         std::to_string(port) + ":active";
}

Result<void> P2PMeshDiscovery::broadcast_beacon(uint16_t beacon_port, uint16_t service_port) {
  const std::string payload = generate_mdns_announcement(service_port);
  auto sock = hal::udp_open("0.0.0.0", 0);
  if (!sock.ok()) return Result<void>(sock.error());
  std::string err;
  ByteSpan span(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  if (!(*sock)->send_to("255.255.255.255", beacon_port, span, err)) return Result<void>(Status::IoError, "broadcast beacon failed: " + err);
  beacons_sent_++;
  return Ok();
}

// Listen on the beacon port for the whole window and take every announcement
// heard: the aios form (AIOS_P2P_MDNS_v1:id:ip:port:state) and the bare-metal
// board's (AIOS-METAL-ARM64 ip mac). A peer is where the datagram came from -
// the sender's address, not the address the payload claims - last seen when it
// was heard. This machine's own beacons are skipped by node id.
Result<size_t> P2PMeshDiscovery::poll_network(int timeout_ms, uint16_t listen_port) {
  auto sock_res = hal::udp_open("0.0.0.0", listen_port);
  if (!sock_res.ok()) return Result<size_t>(sock_res.error());
  auto& sock = *sock_res;
  std::unordered_map<std::string, bool> heard;
  const uint64_t deadline = current_time_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
  for (;;) {
    const uint64_t now = current_time_ms();
    if (now >= deadline) break;
    auto dg = sock->recv_from(2048, (int)(deadline - now));
    if (!dg.ok()) {
      if (dg.error().status == Status::Busy) break;   // the window ran out
      return Result<size_t>(dg.error());
    }
    const std::string payload = rstrip(dg->data);
    DHTPeer peer;
    peer.ip_address = dg->host;
    peer.last_seen_ms = current_time_ms();
    if (payload.rfind("AIOS_P2P_MDNS_v1:", 0) == 0) {
      std::vector<std::string> parts;
      size_t start = 0;
      for (;;) {
        size_t next = payload.find(':', start);
        parts.push_back(payload.substr(start, next == std::string::npos ? std::string::npos : next - start));
        if (next == std::string::npos) break;
        start = next + 1;
      }
      if (parts.size() < 5 || parts[1].empty()) continue;
      peer.node_id = parts[1];
      peer.port = parse_u16(parts[3]);
      peer.is_active = parts[4] == "active";
    } else if (payload.rfind("AIOS-METAL-ARM64 ", 0) == 0) {
      const std::string rest = payload.substr(17);
      const size_t sp = rest.find(' ');
      if (sp == std::string::npos) continue;
      std::string mac;
      for (char c : rest.substr(sp + 1)) if (c != ':') mac += c;
      if (mac.empty()) continue;
      peer.node_id = "rpi3-arm64-" + mac;   // the id the board announces itself under
      peer.port = 7421;
    } else {
      continue;
    }
    if (peer.node_id == local_node_id_str_) continue;
    beacons_heard_++;
    if (add_peer(peer).ok()) heard[peer.node_id] = true;
  }
  return Result<size_t>(heard.size());
}

Result<DHTPeer> P2PMeshDiscovery::parse_mdns_announcement(const std::string& payload) {
  const std::string prefix = "AIOS_P2P_MDNS_v1:";
  if (payload.find(prefix) != 0) {
    return Result<DHTPeer>(Status::Invalid, "Invalid mDNS beacon header format");
  }
  std::vector<std::string> parts;
  size_t start = 0;
  while (start < payload.length()) {
    size_t next = payload.find(':', start);
    if (next == std::string::npos) {
      parts.push_back(payload.substr(start));
      break;
    }
    parts.push_back(payload.substr(start, next - start));
    start = next + 1;
  }
  if (parts.size() < 5) {
    return Result<DHTPeer>(Status::Invalid, "Malformed mDNS beacon payload fields");
  }

  DHTPeer peer;
  peer.node_id = parts[1];
  peer.ip_address = parts[2];
  peer.port = parse_u16(parts[3]);
  peer.last_seen_ms = current_time_ms();
  peer.is_active = (parts[4] == "active");

  auto add_res = add_peer(peer);
  if (!add_res.ok()) return Result<DHTPeer>(add_res.error());

  return Result<DHTPeer>(peer);
}

// Reachable means a TCP connection to ip:port opened within 2 s; only then
// does the peer go in the routing table.
Result<void> P2PMeshDiscovery::ping_peer(const std::string& node_id, const std::string& ip, uint16_t port) {
  auto conn = hal::net_connect(ip, port, 2000);
  if (!conn.ok()) return Result<void>(Status::IoError, ip + ":" + std::to_string(port) + " did not answer: " + conn.error().str());
  (*conn)->close();
  DHTPeer peer;
  peer.node_id = node_id;
  peer.ip_address = ip;
  peer.port = port;
  peer.last_seen_ms = current_time_ms();
  peer.is_active = true;
  return add_peer(peer);
}

Result<void> P2PMeshDiscovery::add_peer(const DHTPeer& peer) {
  if (peer.node_id == local_node_id_str_) {
    return Ok(); // Do not add self to routing table
  }
  peers_[peer.node_id] = peer;
  int idx = get_bucket_index(peer.node_id);
  if (idx >= 0 && idx < static_cast<int>(NUM_BUCKETS)) {
    auto& bucket = k_buckets_[idx];
    auto it = std::find_if(bucket.begin(), bucket.end(),
                           [&](const DHTPeer& p) { return p.node_id == peer.node_id; });
    if (it != bucket.end()) {
      *it = peer;
    } else {
      if (bucket.size() >= K_BUCKET_SIZE) {
        bucket.erase(bucket.begin());
      }
      bucket.push_back(peer);
    }
  }
  return Ok();
}

std::vector<DHTPeer> P2PMeshDiscovery::discover_peers() const {
  std::vector<DHTPeer> list;
  for (const auto& [id, peer] : peers_) {
    list.push_back(peer);
  }
  return list;
}

std::vector<DHTPeer> P2PMeshDiscovery::find_closest_peers(const std::string& target_node_id, size_t count) const {
  NodeID target = hex_to_node_id(target_node_id);
  struct PeerDist {
    DHTPeer peer;
    NodeID dist;
  };
  std::vector<PeerDist> candidates;
  for (const auto& [id, peer] : peers_) {
    NodeID peer_id = hex_to_node_id(peer.node_id);
    candidates.push_back({peer, xor_distance(peer_id, target)});
  }

  std::sort(candidates.begin(), candidates.end(), [](const PeerDist& a, const PeerDist& b) {
    return a.dist < b.dist;
  });

  std::vector<DHTPeer> result;
  for (size_t i = 0; i < std::min(count, candidates.size()); ++i) {
    result.push_back(candidates[i].peer);
  }
  return result;
}

Result<void> P2PMeshDiscovery::rpc_store(const std::string& key, const std::string& value, const std::string& publisher_id) {
  DHTValue val;
  val.key = key;
  val.value = value;
  val.timestamp_ms = current_time_ms();
  val.publisher_id = publisher_id.empty() ? local_node_id_str_ : publisher_id;
  local_storage_[key] = val;
  return Ok();
}

FindValueResult P2PMeshDiscovery::rpc_find_value(const std::string& key) const {
  FindValueResult res;
  auto it = local_storage_.find(key);
  if (it != local_storage_.end()) {
    res.found = true;
    res.value = it->second;
  } else {
    res.found = false;
    NodeID key_node_id = generate_node_id(key);
    res.closest_peers = find_closest_peers(node_id_to_hex(key_node_id), K_BUCKET_SIZE);
  }
  return res;
}

Result<void> P2PMeshDiscovery::announce_resource(const std::string& resource_uri, const std::string& metadata, uint64_t capacity, const std::string& ip, uint16_t port) {
  ResourceAdvertisement ad;
  ad.resource_uri = resource_uri;
  ad.provider_node_id = local_node_id_str_;
  ad.provider_ip = ip;
  ad.provider_port = port;
  ad.metadata = metadata;
  ad.capacity = capacity;
  ad.timestamp_ms = current_time_ms();
  return store_resource(ad);
}

Result<void> P2PMeshDiscovery::store_resource(const ResourceAdvertisement& ad) {
  resources_[ad.resource_uri] = ad;
  // Also index in DHT key-value layer for transparent DHT propagation
  rpc_store("res:" + ad.resource_uri, ad.provider_node_id + "@" + ad.provider_ip + ":" + std::to_string(ad.provider_port) + "|" + ad.metadata, ad.provider_node_id);
  return Ok();
}

ResourceLookupResult P2PMeshDiscovery::find_resource(const std::string& resource_uri) const {
  ResourceLookupResult res;
  auto it = resources_.find(resource_uri);
  if (it != resources_.end()) {
    res.found = true;
    res.resource = it->second;
    return res;
  }

  // Fallback to DHT search
  auto dht_res = rpc_find_value("res:" + resource_uri);
  if (dht_res.found) {
    res.found = true;
    res.resource.resource_uri = resource_uri;
    res.resource.provider_node_id = dht_res.value.publisher_id;
    res.resource.timestamp_ms = dht_res.value.timestamp_ms;
    res.resource.metadata = dht_res.value.value;
  } else {
    res.found = false;
    res.closest_peers = dht_res.closest_peers;
  }
  return res;
}

std::vector<ResourceAdvertisement> P2PMeshDiscovery::list_resources_by_prefix(const std::string& prefix) const {
  std::vector<ResourceAdvertisement> out;
  for (const auto& [uri, ad] : resources_) {
    if (prefix.empty() || uri.rfind(prefix, 0) == 0) {
      out.push_back(ad);
    }
  }
  return out;
}

size_t P2PMeshDiscovery::active_peer_count() const {
  return peers_.size();
}

size_t P2PMeshDiscovery::active_bucket_count() const {
  size_t count = 0;
  for (size_t i = 0; i < NUM_BUCKETS; ++i) {
    if (!k_buckets_[i].empty()) {
      count++;
    }
  }
  return count;
}

int P2PMeshDiscovery::get_bucket_index(const std::string& peer_id) const {
  NodeID p_id = hex_to_node_id(peer_id);
  NodeID dist = xor_distance(local_node_id_, p_id);
  int prefix = common_prefix_length(dist);
  int idx = 159 - prefix;
  if (idx < 0) idx = 0;
  if (idx >= static_cast<int>(NUM_BUCKETS)) idx = NUM_BUCKETS - 1;
  return idx;
}

std::string P2PMeshDiscovery::render_status() const {
  std::string s;
  s += "P2P Mesh Discovery & DHT Status:\n";
  s += "  Local Node ID      : " + local_node_id_str_ + "\n";
  s += "  Local IP Address   : " + local_ip_ + "\n";
  s += "  Beacons            : " + std::to_string(beacons_sent_) + " sent, " + std::to_string(beacons_heard_) +
       " heard by this process (udp/" + std::to_string(kBeaconPort) + ")\n";
  s += "  Total Active Peers : " + std::to_string(active_peer_count()) + "\n";
  s += "  Active K-Buckets   : " + std::to_string(active_bucket_count()) + " / " + std::to_string(NUM_BUCKETS) + "\n";
  s += "  Local DHT Entries  : " + std::to_string(stored_value_count()) + "\n";
  s += "  Tracked Resources  : " + std::to_string(resource_count()) + "\n";
  return s;
}

}  // namespace aios::channel
