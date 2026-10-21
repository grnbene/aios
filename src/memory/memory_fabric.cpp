#include "aios/memory/memory_fabric.hpp"
#include "aios/hal/sync.hpp"
#include <cstring>
#include <functional>

namespace aios {

const char* memory_type_name(MemoryType t) {
  switch (t) {
    case MemoryType::System: return "system";
    case MemoryType::Device: return "device";
    case MemoryType::Shared: return "shared";
    case MemoryType::Persistent: return "persistent";
    case MemoryType::Remote: return "remote";
  }
  return "?";
}
bool parse_memory_type(const std::string& s, MemoryType& out) {
  if (s == "system") out = MemoryType::System;
  else if (s == "device") out = MemoryType::Device;
  else if (s == "shared") out = MemoryType::Shared;
  else if (s == "persistent") out = MemoryType::Persistent;
  else if (s == "remote") out = MemoryType::Remote;
  else return false;
  return true;
}

namespace {
class HostProvider final : public MemoryProvider {
 public:
  HostProvider(hal::Host& h, MemoryType t, hal::MappingKind k, std::string dir) : host_(h), type_(t), kind_(k), dir_(std::move(dir)) {}
  MemoryType type() const override { return type_; }
  Result<hal::Mapping> allocate(uint64_t size, const MemoryID& id) override {
    std::string path;
    if (kind_ == hal::MappingKind::Persistent) {
      AIOS_TRY(host_.make_dir(dir_));
      path = dir_ + "/" + id.str() + ".mem";
    }
    return host_.map(size, kind_, path);
  }
  void release(hal::Mapping& m) override { host_.unmap(m); }
  // A persistent object is a FILE under the root. Unmapping it closes the
  // descriptor and leaves the file, which is right when the kernel is stopping
  // and wrong when the object itself is being destroyed: nothing ever reads
  // that directory back by identity, so the file is unreachable from the
  // moment the object leaves the fabric, and it stays on the disk forever.
  void destroy(hal::Mapping& m) override {
    const std::string path = m.path;
    host_.unmap(m);
    if (kind_ == hal::MappingKind::Persistent && !path.empty()) host_.remove(path);
  }
  uint64_t capacity() const override { return host_.topology().total_memory; }
 private:
  hal::Host& host_;
  MemoryType type_;
  hal::MappingKind kind_;
  std::string dir_;
};
}  // namespace

std::unique_ptr<MemoryProvider> make_system_ram_provider(hal::Host& host) { return std::make_unique<HostProvider>(host, MemoryType::System, hal::MappingKind::Private, ""); }
std::unique_ptr<MemoryProvider> make_shared_memory_provider(hal::Host& host) { return std::make_unique<HostProvider>(host, MemoryType::Shared, hal::MappingKind::Shared, ""); }
std::unique_ptr<MemoryProvider> make_persistent_memory_provider(hal::Host& host, const std::string& dir) { return std::make_unique<HostProvider>(host, MemoryType::Persistent, hal::MappingKind::Persistent, dir); }

MemoryFabric::MemoryFabric(hal::Host& host, IdGenerator& ids) : host_(host), ids_(ids) {}

MemoryFabric::~MemoryFabric() {
  for (auto& [id, obj] : objects_) {
    auto it = providers_.find((uint8_t)obj.type);
    if (it != providers_.end()) it->second->release(obj.mapping);
  }
}

void MemoryFabric::register_provider(std::unique_ptr<MemoryProvider> p) {
  hal::LockGuard g(mu_);
  providers_[(uint8_t)p->type()] = std::move(p);
}
bool MemoryFabric::has_provider(MemoryType t) const { hal::LockGuard g(mu_); return providers_.count((uint8_t)t) > 0; }
MemoryProvider* MemoryFabric::provider(MemoryType t) const {
  hal::LockGuard g(mu_);
  auto it = providers_.find((uint8_t)t);
  return it == providers_.end() ? nullptr : it->second.get();
}

Result<MemoryID> MemoryFabric::allocate(const EntityID& owner, uint64_t size, MemoryType type, uint8_t perms) {
  MemoryID id = ids_.next_as<MemoryID>();
  return allocate_with_id(id, owner, size, type, perms, 0);
}

Result<MemoryID> MemoryFabric::allocate_with_id(const MemoryID& id, const EntityID& owner, uint64_t size, MemoryType type, uint8_t perms, uint64_t generation) {
  if (size == 0) return Result<MemoryID>(Status::Invalid, "zero-size memory object");
  hal::LockGuard g(mu_);
  if (objects_.count(id)) return Result<MemoryID>(Status::Conflict, "memory identity already present");
  auto it = providers_.find((uint8_t)type);
  if (it == providers_.end()) return Result<MemoryID>(Status::Unsupported, std::string("no provider for ") + memory_type_name(type) + " memory");
  auto m = it->second->allocate(size, id);
  if (!m.ok()) return m.error();
  MemoryObject obj;
  obj.id = id;
  obj.size = size;
  obj.type = type;
  obj.location.type = type;
  obj.permissions = perms;
  obj.owner = owner;
  obj.generation = generation;
  obj.created_ns = host_.wall_ns();
  obj.mapping = *m;
  objects_[id] = obj;
  return id;
}

Result<void> MemoryFabric::release(const MemoryID& id) {
  hal::LockGuard g(mu_);
  auto it = objects_.find(id);
  if (it == objects_.end()) return Result<void>(Status::NotFound, "memory " + id.short_str());
  auto p = providers_.find((uint8_t)it->second.type);
  if (p != providers_.end()) p->second->destroy(it->second.mapping);
  objects_.erase(it);
  return Ok();
}

Result<void> MemoryFabric::release_owned(const EntityID& owner) {
  std::vector<MemoryID> ids;
  {
    hal::LockGuard g(mu_);
    for (auto& [id, o] : objects_) if (o.owner == owner) ids.push_back(id);
  }
  for (auto& id : ids) release(id);
  return Ok();
}

const MemoryObject* MemoryFabric::lookup(const MemoryID& id) const {
  hal::LockGuard g(mu_);
  auto it = objects_.find(id);
  return it == objects_.end() ? nullptr : &it->second;
}

Result<void> MemoryFabric::write(const MemoryID& id, uint64_t offset, ByteSpan data) {
  hal::LockGuard g(mu_);
  auto it = objects_.find(id);
  if (it == objects_.end()) return Result<void>(Status::NotFound, "memory " + id.short_str());
  if (offset + data.size() > it->second.size) return Result<void>(Status::Invalid, "write past end of memory object");
  std::memcpy(static_cast<uint8_t*>(it->second.base()) + offset, data.data(), data.size());
  it->second.generation++;
  if (it->second.type == MemoryType::Persistent) host_.sync(it->second.mapping);
  return Ok();
}

Result<ByteBuffer> MemoryFabric::read(const MemoryID& id, uint64_t offset, uint64_t length) const {
  hal::LockGuard g(mu_);
  auto it = objects_.find(id);
  if (it == objects_.end()) return Result<ByteBuffer>(Status::NotFound, "memory " + id.short_str());
  if (offset > it->second.size) return Result<ByteBuffer>(Status::Invalid, "read past end");
  if (offset + length > it->second.size) length = it->second.size - offset;
  const uint8_t* p = static_cast<const uint8_t*>(it->second.base()) + offset;
  return ByteBuffer(p, p + length);
}

Result<void> MemoryFabric::fill(const MemoryID& id, uint8_t byte) {
  hal::LockGuard g(mu_);
  auto it = objects_.find(id);
  if (it == objects_.end()) return Result<void>(Status::NotFound);
  std::memset(it->second.base(), byte, it->second.size);
  it->second.generation++;
  return Ok();
}

Result<void> MemoryFabric::transfer_owner(const MemoryID& id, const EntityID& new_owner) {
  hal::LockGuard g(mu_);
  auto it = objects_.find(id);
  if (it == objects_.end()) return Result<void>(Status::NotFound);
  it->second.owner = new_owner;
  it->second.generation++;
  return Ok();
}

Result<void> MemoryFabric::migrate(const MemoryID& id, MemoryType to) {
  hal::LockGuard g(mu_);
  auto it = objects_.find(id);
  if (it == objects_.end()) return Result<void>(Status::NotFound, "memory " + id.short_str());
  MemoryObject& obj = it->second;
  if (obj.type == to) return Ok();
  auto p = providers_.find((uint8_t)to);
  if (p == providers_.end()) return Result<void>(Status::Unsupported, std::string("no provider for ") + memory_type_name(to));
  auto m = p->second->allocate(obj.size, obj.id);
  if (!m.ok()) return m.error();
  std::memcpy(m->base, obj.base(), obj.size);
  // The old placement is gone for good, not merely unmapped: destroy, so a
  // persistent object migrated away takes its .mem file with it (release only
  // closed it, and the file stayed under the root forever).
  auto old = providers_.find((uint8_t)obj.type);
  if (old != providers_.end()) old->second->destroy(obj.mapping);
  obj.mapping = *m;
  obj.type = to;
  obj.location.type = to;
  obj.generation++;
  migrations_++;
  return Ok();
}

Result<MemoryFabric::Window> MemoryFabric::window(const MemoryID& id) {
  hal::LockGuard g(mu_);
  auto it = objects_.find(id);
  if (it == objects_.end()) return Result<Window>(Status::NotFound, "memory " + id.short_str());
  return Window{static_cast<uint8_t*>(it->second.base()), it->second.size};
}

void MemoryFabric::touch(const MemoryID& id) {
  hal::LockGuard g(mu_);
  auto it = objects_.find(id);
  if (it != objects_.end()) it->second.generation++;
}

std::vector<MemoryObject> MemoryFabric::list(const EntityID& owner) const {
  hal::LockGuard g(mu_);
  std::vector<MemoryObject> out;
  for (auto& [id, o] : objects_) if (owner.is_null() || o.owner == owner) out.push_back(o);
  return out;
}

MemoryStats MemoryFabric::stats() const {
  hal::LockGuard g(mu_);
  MemoryStats s;
  for (auto& [id, o] : objects_) { s.objects++; s.bytes += o.size; s.bytes_by_type[(uint8_t)o.type] += o.size; }
  s.migrations = migrations_;
  return s;
}

MemoryFabric::CompactResult MemoryFabric::compact_garbage(OwnerAlive owner_alive) {
  hal::LockGuard g(mu_);
  CompactResult res;
  res.objects_checked = objects_.size();
  std::vector<MemoryID> to_remove;
  for (auto& [id, obj] : objects_) {
    // An object is garbage when nothing can name its owner any more. It is NOT
    // garbage for having been written to a lot: `generation > 1000` counted
    // WRITES, not idleness, so the busiest object on the machine was the first
    // one this freed out from under its owner.
    const bool orphaned = obj.owner.is_null() || (owner_alive && !owner_alive(obj.owner));
    if (orphaned || obj.size == 0) to_remove.push_back(id);
  }
  for (const auto& id : to_remove) {
    auto it = objects_.find(id);
    if (it != objects_.end()) {
      res.bytes_reclaimed += it->second.size;
      auto p = providers_.find((uint8_t)it->second.type);
      if (p != providers_.end()) {
        p->second->destroy(it->second.mapping);
      }
      objects_.erase(it);
      res.objects_released++;
    }
  }
  return res;
}

}  // namespace aios
