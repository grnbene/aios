#include "aios/channel/state_channel.hpp"
#include "aios/hal/sync.hpp"
#include <algorithm>

namespace aios {

ChannelManager::ChannelManager(IdGenerator& ids, MemoryFabric& fabric) : ids_(ids), fabric_(fabric) {}

void ChannelManager::register_transport(std::unique_ptr<Transport> t) {
  hal::LockGuard g(mu_);
  transports_[t->name()] = std::move(t);
}

Result<ChannelID> ChannelManager::create(const EntityID& owner, uint32_t capacity, uint64_t slot_bytes, uint64_t now_ns, const std::string& transport) {
  if (capacity == 0 || slot_bytes == 0) return Result<ChannelID>(Status::Invalid, "channel needs capacity and slot size");
  Transport* t;
  {
    hal::LockGuard g(mu_);
    auto it = transports_.find(transport);
    if (it == transports_.end()) return Result<ChannelID>(Status::Unsupported, "no transport '" + transport + "'");
    t = it->second.get();
  }
  uint64_t bytes = t->region_bytes(capacity, slot_bytes);
  MemoryType type = fabric_.has_provider(MemoryType::Shared) ? MemoryType::Shared : MemoryType::System;
  auto region = fabric_.allocate(owner, bytes, type, MemRead | MemWrite | MemShare);
  if (!region.ok()) return region.error();
  auto f = t->format(fabric_, *region, capacity, slot_bytes);
  if (!f.ok()) { fabric_.release(*region); return f.error(); }
  Channel ch;
  ch.id = ids_.next_as<ChannelID>();
  ch.owner = owner; ch.region = *region; ch.capacity = capacity; ch.slot_bytes = slot_bytes; ch.created_ns = now_ns; ch.transport = transport;
  hal::LockGuard g(mu_);
  channels_[ch.id] = ch;
  return ch.id;
}

Result<uint64_t> ChannelManager::region_bytes(uint32_t capacity, uint64_t slot_bytes, const std::string& transport) const {
  hal::LockGuard g(mu_);
  auto it = transports_.find(transport);
  if (it == transports_.end()) return Result<uint64_t>(Status::Unsupported, "no transport '" + transport + "'");
  return it->second->region_bytes(capacity, slot_bytes);
}

Result<void> ChannelManager::subscribe(const ChannelID& id, const EntityID& consumer) {
  hal::LockGuard g(mu_);
  auto it = channels_.find(id);
  if (it == channels_.end()) return Result<void>(Status::NotFound, "channel " + id.short_str());
  for (auto& c : it->second.cursors) if (c.consumer == consumer) return Ok();
  uint64_t head = 0, tail = 0, dropped = 0;
  transports_[it->second.transport]->stats(fabric_, it->second.region, head, tail, dropped);
  // A new subscriber sees everything still resident in the ring.
  it->second.cursors.push_back(ChannelCursor{consumer, tail, 0});
  return Ok();
}

Result<void> ChannelManager::unsubscribe(const ChannelID& id, const EntityID& consumer) {
  hal::LockGuard g(mu_);
  auto it = channels_.find(id);
  if (it == channels_.end()) return Result<void>(Status::NotFound, "channel " + id.short_str());
  auto& cs = it->second.cursors;
  cs.erase(std::remove_if(cs.begin(), cs.end(), [&](const ChannelCursor& c) { return c.consumer == consumer; }), cs.end());
  return Ok();
}

Result<uint64_t> ChannelManager::publish(const ChannelID& id, const EntityID& publisher, const StateObject& state, uint64_t now_ns) {
  hal::LockGuard g(mu_);
  auto it = channels_.find(id);
  if (it == channels_.end()) return Result<uint64_t>(Status::NotFound, "channel " + id.short_str());
  StateFrame f;
  f.publisher = publisher; f.state = state.id; f.kind = state.kind; f.generation = state.generation; f.published_ns = now_ns;
  f.payload = state.payload; f.memory = state.memory;
  auto r = transports_[it->second.transport]->publish(fabric_, it->second.region, f);
  if (r.ok()) it->second.published++;
  return r;
}

Result<StateFrame> ChannelManager::consume(const ChannelID& id, const EntityID& consumer) {
  hal::LockGuard g(mu_);
  auto it = channels_.find(id);
  if (it == channels_.end()) return Result<StateFrame>(Status::NotFound, "channel " + id.short_str());
  for (auto& c : it->second.cursors) {
    if (c.consumer == consumer) return transports_[it->second.transport]->consume(fabric_, it->second.region, c);
  }
  return Result<StateFrame>(Status::Denied, "entity " + consumer.short_str() + " is not subscribed to channel " + id.short_str());
}

Result<void> ChannelManager::destroy(const ChannelID& id) {
  hal::LockGuard g(mu_);
  auto it = channels_.find(id);
  if (it == channels_.end()) return Result<void>(Status::NotFound, "channel " + id.short_str());
  fabric_.release(it->second.region);
  channels_.erase(it);
  return Ok();
}

Result<void> ChannelManager::install(const Channel& c) {
  hal::LockGuard g(mu_);
  if (!transports_.count(c.transport)) return Result<void>(Status::Unsupported, "no transport '" + c.transport + "'");
  if (!fabric_.lookup(c.region)) return Result<void>(Status::NotFound, "channel region " + c.region.short_str());
  channels_[c.id] = c;
  return Ok();
}

Result<void> ChannelManager::destroy_owned(const EntityID& owner) {
  std::vector<ChannelID> ids;
  { hal::LockGuard g(mu_); for (auto& [id, c] : channels_) if (c.owner == owner) ids.push_back(id); }
  for (auto& id : ids) destroy(id);
  return Ok();
}

const Channel* ChannelManager::get(const ChannelID& id) const {
  hal::LockGuard g(mu_);
  auto it = channels_.find(id);
  return it == channels_.end() ? nullptr : &it->second;
}
std::vector<Channel> ChannelManager::list() const {
  hal::LockGuard g(mu_);
  std::vector<Channel> out;
  for (auto& [id, c] : channels_) out.push_back(c);
  return out;
}
Result<void> ChannelManager::stats(const ChannelID& id, uint64_t& head, uint64_t& tail, uint64_t& dropped) {
  hal::LockGuard g(mu_);
  auto it = channels_.find(id);
  if (it == channels_.end()) return Result<void>(Status::NotFound, "channel " + id.short_str());
  return transports_[it->second.transport]->stats(fabric_, it->second.region, head, tail, dropped);
}

}  // namespace aios

namespace aios {
// Restore path: registers a channel record whose region already exists.
Result<void> channel_manager_install(ChannelManager& cm, const Channel& c) { return cm.install(c); }
}  // namespace aios
