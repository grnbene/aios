// Capture and restore of an entity: not a dump of process RAM but the entity's
// identity, state objects, memory contents, capabilities, graphs, channels,
// relationships and execution position, in the kernel's native encoding.
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"

namespace aios {

// Forward-declared at namespace scope, not inside restore() where it is called: a function-scope
// `extern` declaration of a free function is block-scoped-but-externally-linked on every compiler,
// but MSVC mangles it into the GLOBAL namespace rather than the enclosing one (clang/gcc both give it
// aios::channel_manager_install, matching channel_manager.cpp's real definition) — declaring it here
// instead removes the ambiguity rather than relying on a corner case the compilers disagree on.
extern Result<void> channel_manager_install(ChannelManager&, const Channel&);

static constexpr uint32_t CKPT_MAGIC = 0x434B4F41u;  // 'AOKC'
// 2: an operation carries its device requirement as written (device_selector)
//    instead of only a resolved instance, so a graph stays runnable after it
//    moves to a kernel whose devices are different objects.
static constexpr uint32_t CKPT_VERSION = 2;

Result<ByteBuffer> Kernel::capture(const EntityID& id) {
  auto e = entities_->copy(id);
  if (!e.ok()) return e.error();
  ByteBuffer body;
  Writer w(body);
  w.id(self_);
  w.u64(now());
  e->encode(w);
  // States
  auto states = states_->list(id);
  w.u32((uint32_t)states.size());
  for (auto& s : states) s.encode(w);
  // Memory objects owned, with contents
  auto mem = memory_->list(id);
  w.u32((uint32_t)mem.size());
  for (auto& m : mem) {
    w.id(m.id); w.u64(m.size); w.u8((uint8_t)m.type); w.u8(m.permissions); w.u64(m.generation); w.u64(m.created_ns);
    auto bytes = memory_->read(m.id, 0, m.size);
    if (!bytes.ok()) return bytes.error();
    w.bytes(*bytes);
  }
  // Capabilities held
  auto caps = caps_->list(id);
  w.u32((uint32_t)caps.size());
  for (auto& c : caps) c.encode(w);
  // Graphs owned
  auto graphs = graphs_->list(id);
  w.u32((uint32_t)graphs.size());
  for (auto& g : graphs) g.encode(w);
  // Channels owned (their regions are among the memory objects above)
  std::vector<Channel> chans;
  for (auto& c : channels_->list()) if (c.owner == id) chans.push_back(c);
  w.u32((uint32_t)chans.size());
  for (auto& c : chans) {
    w.id(c.id); w.id(c.region); w.u32(c.capacity); w.u64(c.slot_bytes); w.u64(c.created_ns); w.u64(c.published); w.str(c.transport);
    w.u32((uint32_t)c.cursors.size());
    for (auto& cur : c.cursors) { w.id(cur.consumer); w.u64(cur.next_sequence); w.u64(cur.lost); }
  }
  ByteBuffer image;
  Writer iw(image);
  iw.u32(CKPT_MAGIC); iw.u32(CKPT_VERSION); iw.u64(body.size()); iw.u64(fnv1a(body)); iw.raw(body);
  return image;
}

Result<std::string> Kernel::checkpoint(const EntityID& id) {
  if (!entities_->exists(id)) return Result<std::string>(Status::NotFound, "entity " + id.short_str());
  // The image records this checkpoint as taken, so a restore knows its lineage.
  entities_->note_checkpoint(id);
  auto image = capture(id);
  if (!image.ok()) return image.error();
  auto e = entities_->copy(id);
  std::string dir = cfg_.root + "/checkpoints/" + id.str();
  AIOS_TRY(host_->make_dir(dir));
  // Numbered past whatever is ALREADY on disk, not off this kernel's own
  // counter. The counter starts at zero in a kernel that has just booted, so a
  // second kernel on the same root — the desktop beside the resident, which is
  // how this machine actually runs — wrote 1.aoc over a checkpoint that was
  // already there. A moment the machine could have been put back to was
  // destroyed to record a new one, silently, and the path it returned looked
  // exactly like success.
  uint64_t next = e->checkpoints;
  if (auto have = host_->list_dir(dir); have.ok()) {
    for (const auto& f : *have) {
      if (f.size() < 5 || f.substr(f.size() - 4) != ".aoc") continue;
      uint64_t n = 0;
      for (char c : f) { if (c < '0' || c > '9') break; n = n * 10 + (uint64_t)(c - '0'); }
      if (n >= next) next = n + 1;
    }
  }
  std::string path = dir + "/" + std::to_string(next) + ".aoc";
  AIOS_TRY(host_->write_file(path, *image));
  return path;
}

Result<EntityID> Kernel::restore(const std::string& path, const EntityID& restorer) {
  auto data = host_->read_file(path);
  if (!data.ok()) return data.error();
  return restore_image(*data, restorer);
}

Result<EntityID> Kernel::rewind(const std::string& path, const EntityID& actor) {
  auto data = host_->read_file(path);
  if (!data.ok()) return data.error();

  // Verify the image and find out whose it is BEFORE touching the live entity.
  // Tearing down first and reading second would mean a corrupt file costs the
  // very thing the rewind was for.
  Reader hr(*data);
  const uint32_t magic = hr.u32(), version = hr.u32();
  const uint64_t len = hr.u64(), sum = hr.u64();
  if (!hr.ok() || magic != CKPT_MAGIC) return Result<EntityID>(Status::Corrupt, "not a checkpoint image");
  if (version != CKPT_VERSION) return Result<EntityID>(Status::Unsupported, "checkpoint version " + std::to_string(version));
  if (len > hr.remaining()) return Result<EntityID>(Status::Corrupt, "truncated checkpoint");
  ByteSpan body = hr.raw(len);
  if (fnv1a(body) != sum) return Result<EntityID>(Status::Corrupt, "checkpoint checksum mismatch");
  Reader r(body);
  r.id();      // the kernel it was taken on
  r.u64();     // when
  auto image_entity = Entity::decode(r);
  if (!image_entity.ok()) return image_entity.error();
  const EntityID id = image_entity->id;

  // Nothing live to put back over: this is an ordinary restore.
  if (!entities_->exists(id)) return restore_image(*data, actor);
  if (id == self_) return Result<EntityID>(Status::Denied, "the kernel cannot be rewound from inside itself");

  note(AuthorityEvent::Revoked, actor, id, ResourceRef{ResourceKind::Entity, id}, 0, {},
       "rewound to a checkpoint: everything it held was released first");

  // Everything it holds goes. The identity does not: the image reinstalls the
  // entity under the same id, which is the whole difference between this and
  // terminating it.
  unbind_mind(id);
  // Its memory goes back to whichever budgets paid for it (a parent that
  // delegated it scope gets that scope back).
  if (const Entity* ent = entities_->get(id)) for (auto& m : ent->memory) caps_->refund_object(m);
  channels_->destroy_owned(id);
  graphs_->destroy_owned(id);
  states_->destroy_owned(id);
  memory_->release_owned(id);
  caps_->revoke_all(id);
  AIOS_TRY(entities_->remove(id));

  return restore_image(*data, actor);
}

Result<EntityID> Kernel::restore_image(ByteSpan image, const EntityID& restorer) {
  Reader hr(image);
  uint32_t magic = hr.u32(), version = hr.u32();
  uint64_t len = hr.u64(), sum = hr.u64();
  if (!hr.ok() || magic != CKPT_MAGIC) return Result<EntityID>(Status::Corrupt, "not a checkpoint image");
  if (version != CKPT_VERSION) return Result<EntityID>(Status::Unsupported, "checkpoint version " + std::to_string(version));
  if (len > hr.remaining()) return Result<EntityID>(Status::Corrupt, "truncated checkpoint");
  ByteSpan body = hr.raw(len);
  if (fnv1a(body) != sum) return Result<EntityID>(Status::Corrupt, "checkpoint checksum mismatch");
  Reader r(body);
  Id128 origin_kernel = r.id();
  (void)origin_kernel;
  r.u64();
  auto e = Entity::decode(r);
  if (!e.ok()) return e.error();
  if (entities_->exists(e->id)) return Result<EntityID>(Status::Conflict, "entity " + e->id.short_str() + " is already present in this kernel; terminate it before restoring");

  uint32_t n = r.u32();
  std::vector<StateObject> states;
  for (uint32_t i = 0; i < n && r.ok(); ++i) { auto s = StateObject::decode(r); if (!s.ok()) return s.error(); states.push_back(*s); }
  struct MemRec { MemoryID id; uint64_t size; MemoryType type; uint8_t perms; uint64_t gen, created; ByteSpan data; };
  std::vector<MemRec> mems;
  n = r.u32();
  for (uint32_t i = 0; i < n && r.ok(); ++i) {
    MemRec m; m.id = MemoryID{r.id()}; m.size = r.u64(); m.type = (MemoryType)r.u8(); m.perms = r.u8(); m.gen = r.u64(); m.created = r.u64(); m.data = r.bytes();
    if (!r.ok() || m.data.size() != m.size) return Result<EntityID>(Status::Corrupt, "memory record");
    mems.push_back(m);
  }
  std::vector<Capability> caps;
  n = r.u32();
  for (uint32_t i = 0; i < n && r.ok(); ++i) { auto c = Capability::decode(r); if (!c.ok()) return c.error(); caps.push_back(*c); }
  std::vector<ComputeGraph> graphs;
  n = r.u32();
  for (uint32_t i = 0; i < n && r.ok(); ++i) { auto g = ComputeGraph::decode(r); if (!g.ok()) return g.error(); graphs.push_back(*g); }
  struct ChanRec { Channel ch; };
  std::vector<Channel> chans;
  n = r.u32();
  for (uint32_t i = 0; i < n && r.ok(); ++i) {
    Channel c; c.id = ChannelID{r.id()}; c.region = MemoryID{r.id()}; c.capacity = r.u32(); c.slot_bytes = r.u64(); c.created_ns = r.u64(); c.published = r.u64(); c.transport = r.str();
    uint32_t nc = r.u32();
    for (uint32_t j = 0; j < nc && r.ok(); ++j) { ChannelCursor cur; cur.consumer = EntityID{r.id()}; cur.next_sequence = r.u64(); cur.lost = r.u64(); c.cursors.push_back(cur); }
    c.owner = e->id;
    chans.push_back(c);
  }
  if (!r.ok()) return Result<EntityID>(Status::Corrupt, "checkpoint body");

  // Materialise. Identity is preserved for every object.
  Entity ent = *e;
  ent.status = EntityStatus::Active;
  // Relationships to entities that no longer exist here are kept: identity is
  // what matters, and the peer may be restored later.
  AIOS_TRY(entities_->install(ent));
  for (auto& m : mems) {
    MemoryType t = memory_->has_provider(m.type) ? m.type : MemoryType::System;
    auto a = memory_->allocate_with_id(m.id, ent.id, m.size, t, m.perms, m.gen);
    if (!a.ok()) {
      // Half a restore is not a restore. Everything already allocated under
      // this identity goes back before the entity does -- removing the entity
      // first left those objects owned by something that no longer exists,
      // which no release_owned can ever reach again.
      memory_->release_owned(ent.id);
      entities_->remove(ent.id);
      return a.error();
    }
    memory_->write(m.id, 0, m.data);
  }
  for (auto& s : states) states_->install(s);
  if (restorer.is_null() || restorer == self_) {
    for (auto& c : caps) caps_->install(c);
  } else {
    // Authority does not travel in an image. The entity keeps authority over
    // what the image itself recreates (its own memory, states, channels, self);
    // everything else must be derivable from the RESTORER's authority, and is
    // re-issued as a delegation from it — or dropped, on the record.
    std::vector<CapabilityID> kept;
    for (auto& c : caps) {
      bool own = false;
      if (!c.resource.id.is_null()) {
        switch (c.resource.kind) {
          case ResourceKind::Memory: for (auto& m : mems) own |= m.id == c.resource.id; break;
          case ResourceKind::State: for (auto& s : states) own |= s.id == c.resource.id; break;
          case ResourceKind::Channel: for (auto& ch : chans) own |= ch.id == c.resource.id; break;
          case ResourceKind::Entity: own = c.resource.id == ent.id; break;
          default: break;
        }
      }
      if (own) { if (caps_->install(c).ok()) kept.push_back(c.id); continue; }
      CapabilityID source;
      uint16_t grantable = 0;
      for (auto& rc : caps_->list(restorer)) {
        if (!rc.resource.covers(c.resource) || !(rc.rights & R_Grant)) continue;
        if (rc.expires_ns && now() >= rc.expires_ns) continue;
        uint16_t common = c.rights & rc.rights;
        if (common == 0) continue;
        if (c.scope_limit && rc.scope_limit && c.scope_limit > rc.scope_limit) continue;
        source = rc.id;
        grantable = common;
        break;
      }
      if (source.is_null()) {
        note(AuthorityEvent::Attenuated, restorer, ent.id, c.resource, c.rights, c.id, "restore dropped it: the restorer holds no such delegable authority");
        continue;
      }
      auto d = caps_->delegate(source, ent.id, grantable, now(), c.expires_ns, c.scope_limit, &c.resource);
      if (d.ok()) { kept.push_back(*d); note(AuthorityEvent::Attenuated, restorer, ent.id, c.resource, grantable, *d, "restore re-derived it from the restorer's " + source.short_str()); }
    }
    Entity fixed = ent; fixed.capabilities = kept;
    entities_->remove(ent.id);
    entities_->install(fixed);
  }
  for (auto& g : graphs) graphs_->install(g);
  for (auto& c : chans) {
    // Rebind the channel record to its (restored) region. The ring is intact
    // because the region's bytes were captured verbatim.
    channels_->destroy(c.id);  // no-op if absent
    // ChannelManager has no install(); recreate through its internals by
    // creating a fresh channel would change identity, so we register directly.
    channel_manager_install(*channels_, c);
  }
  entities_->note_restore(ent.id);
  return ent.id;
}

}  // namespace aios
