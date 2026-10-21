#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "aios/core/result.hpp"

namespace aios::channel {

// 160-bit Kademlia Node ID representation (20 bytes)
using NodeID = std::array<uint8_t, 20>;

NodeID hex_to_node_id(const std::string& hex);
std::string node_id_to_hex(const NodeID& id);
NodeID xor_distance(const NodeID& a, const NodeID& b);
int common_prefix_length(const NodeID& distance);
NodeID generate_node_id(const std::string& seed);

struct DHTPeer {
  std::string node_id;
  std::string ip_address;
  uint16_t port = 8080;
  uint64_t last_seen_ms = 0;
  bool is_active = true;
};

struct DHTValue {
  std::string key;
  std::string value;
  uint64_t timestamp_ms = 0;
  std::string publisher_id;
};

struct FindValueResult {
  bool found = false;
  DHTValue value;
  std::vector<DHTPeer> closest_peers;
};

struct ResourceAdvertisement {
  std::string resource_uri;  // e.g. "Object/abc123", "Model/gemma", "Compute/ERP", "Entity/902628"
  std::string provider_node_id;
  std::string provider_ip;
  uint16_t provider_port = 0;
  std::string metadata;      // Type info, schema, capacity description
  uint64_t capacity = 0;     // e.g. VRAM, lanes, size, or generation
  uint64_t timestamp_ms = 0;
};

struct ResourceLookupResult {
  bool found = false;
  ResourceAdvertisement resource;
  std::vector<DHTPeer> closest_peers;
};

class P2PMeshDiscovery {
 public:
  static constexpr size_t K_BUCKET_SIZE = 8;
  static constexpr size_t NUM_BUCKETS = 160;

  P2PMeshDiscovery();
  explicit P2PMeshDiscovery(const std::string& node_id);

  const std::string& local_node_id() const { return local_node_id_str_; }
  const std::string& local_ip() const { return local_ip_; }
  void set_local_ip(const std::string& ip) { local_ip_ = ip; }

  static constexpr uint16_t kBeaconPort = 42423;   // the LAN beacon port (the metal board beacons here too)
  // One announcement to 255.255.255.255:beacon_port.
  Result<void> broadcast_beacon(uint16_t beacon_port = kBeaconPort, uint16_t service_port = 7421);
  // Listen for timeout_ms, adding every peer heard; returns how many distinct peers.
  Result<size_t> poll_network(int timeout_ms, uint16_t listen_port = kBeaconPort);
  uint64_t beacons_sent() const { return beacons_sent_; }
  uint64_t beacons_heard() const { return beacons_heard_; }

  Result<void> start_mdns_beacon(uint16_t port);
  Result<void> stop_mdns_beacon();
  bool is_mdns_active() const { return mdns_active_; }

  std::string generate_mdns_announcement(uint16_t port) const;
  Result<DHTPeer> parse_mdns_announcement(const std::string& payload);

  Result<void> ping_peer(const std::string& node_id, const std::string& ip, uint16_t port);
  Result<void> add_peer(const DHTPeer& peer);

  std::vector<DHTPeer> discover_peers() const;
  std::vector<DHTPeer> find_closest_peers(const std::string& target_node_id, size_t count = K_BUCKET_SIZE) const;

  Result<void> rpc_store(const std::string& key, const std::string& value, const std::string& publisher_id = "");
  FindValueResult rpc_find_value(const std::string& key) const;

  // Resource discovery (Object/*, Model/*, Compute/*, Entity/*, Service/*)
  Result<void> announce_resource(const std::string& resource_uri, const std::string& metadata = "", uint64_t capacity = 0, const std::string& ip = "127.0.0.1", uint16_t port = 0);
  Result<void> store_resource(const ResourceAdvertisement& ad);
  ResourceLookupResult find_resource(const std::string& resource_uri) const;
  std::vector<ResourceAdvertisement> list_resources_by_prefix(const std::string& prefix) const;

  size_t active_peer_count() const;
  size_t active_bucket_count() const;
  size_t stored_value_count() const { return local_storage_.size(); }
  size_t resource_count() const { return resources_.size(); }

  std::string render_status() const;

 private:
  int get_bucket_index(const std::string& peer_id) const;

  NodeID local_node_id_;
  std::string local_node_id_str_;
  std::string local_ip_ = "127.0.0.1";
  std::unordered_map<std::string, DHTPeer> peers_;
  std::array<std::vector<DHTPeer>, NUM_BUCKETS> k_buckets_;
  std::unordered_map<std::string, DHTValue> local_storage_;
  std::unordered_map<std::string, ResourceAdvertisement> resources_;
  bool mdns_active_ = false;   // true once a beacon actually went out
  uint64_t beacons_sent_ = 0, beacons_heard_ = 0;
  uint16_t mdns_port_ = 5353;
};

}  // namespace aios::channel
