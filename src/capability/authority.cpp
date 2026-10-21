#include "aios/capability/authority.hpp"

namespace aios {

namespace {
constexpr uint32_t AUTH_MAGIC = 0x48545541u;   // "AUTH"
enum RecType : uint8_t { RT_Resident = 1, RT_Capability = 2, RT_Revoked = 3, RT_Event = 4 };
}

const char* authority_event_name(AuthorityEvent::Kind k) {
  switch (k) {
    case AuthorityEvent::Minted: return "minted"; case AuthorityEvent::Derived: return "derived";
    case AuthorityEvent::Revoked: return "revoked"; case AuthorityEvent::Requested: return "requested";
    case AuthorityEvent::Refused: return "REFUSED"; case AuthorityEvent::Admitted: return "admitted";
    case AuthorityEvent::Attenuated: return "attenuated";
    case AuthorityEvent::Exercised: return "exercised";
  }
  return "?";
}

AuthorityLedger::AuthorityLedger(hal::Host& host, std::string path) : host_(host), path_(std::move(path)) {}

Result<void> AuthorityLedger::open() {
  residents_.clear(); caps_.clear(); events_.clear();
  auto data = host_.read_file(path_);
  if (!data.ok()) { persisted_ = false; return Ok(); }   // first boot at this root
  persisted_ = true;
  Reader r(*data);
  while (r.remaining() >= 9) {
    uint32_t magic = r.u32(); uint8_t type = r.u8(); uint32_t len = r.u32();
    if (!r.ok() || magic != AUTH_MAGIC || len > r.remaining()) break;   // torn tail: keep what was complete
    ByteSpan body = r.raw(len);
    Reader b(body);
    switch (type) {
      case RT_Resident: { Resident res; res.label = b.str(); res.id = EntityID{b.id()}; res.created_ns = b.u64(); if (b.remaining()) res.self_improve = b.u8() != 0; if (b.ok()) residents_.push_back(res); break; }
      case RT_Capability: { auto c = Capability::decode(b); if (c.ok()) { bool dup = false; for (auto& e : caps_) if (e.id == c->id) { e = *c; dup = true; } if (!dup) caps_.push_back(*c); } break; }
      case RT_Revoked: { CapabilityID id{b.id()}; for (size_t i = 0; i < caps_.size(); ++i) if (caps_[i].id == id) { caps_.erase(caps_.begin() + i); break; } break; }
      case RT_Event: {
        AuthorityEvent e; e.kind = (AuthorityEvent::Kind)b.u8(); e.ns = b.u64(); e.actor = EntityID{b.id()}; e.subject = EntityID{b.id()};
        e.resource.kind = (ResourceKind)b.u8(); e.resource.id = b.id(); e.rights = b.u16(); e.capability = CapabilityID{b.id()}; e.note = b.str();
        if (b.ok()) events_.push_back(e);
        break;
      }
      default: break;
    }
  }
  return Ok();
}

void AuthorityLedger::append(uint8_t type, const ByteBuffer& payload) {
  ByteBuffer rec; Writer w(rec);
  w.u32(AUTH_MAGIC); w.u8(type); w.u32((uint32_t)payload.size()); w.raw(ByteSpan(payload.data(), payload.size()));
  host_.append_file(path_, rec);
  persisted_ = true;
}

void AuthorityLedger::record_resident(const Resident& r) {
  residents_.push_back(r);
  ByteBuffer b; Writer w(b); w.str(r.label); w.id(r.id); w.u64(r.created_ns); w.u8(r.self_improve ? 1 : 0);
  append(RT_Resident, b);
}
void AuthorityLedger::record_capability(const Capability& c) {
  bool dup = false; for (auto& e : caps_) if (e.id == c.id) { e = c; dup = true; }
  if (!dup) caps_.push_back(c);
  ByteBuffer b; Writer w(b); c.encode(w);
  append(RT_Capability, b);
}
void AuthorityLedger::record_revoked(const CapabilityID& id) {
  bool had = false;
  for (size_t i = 0; i < caps_.size(); ++i) if (caps_[i].id == id) { caps_.erase(caps_.begin() + i); had = true; break; }
  if (!had) return;   // not a resident's capability: nothing persisted to retract
  ByteBuffer b; Writer w(b); w.id(id);
  append(RT_Revoked, b);
}
void AuthorityLedger::record_event(const AuthorityEvent& e) {
  events_.push_back(e);
  ByteBuffer b; Writer w(b);
  w.u8((uint8_t)e.kind); w.u64(e.ns); w.id(e.actor); w.id(e.subject); w.u8((uint8_t)e.resource.kind); w.id(e.resource.id); w.u16(e.rights); w.id(e.capability); w.str(e.note);
  append(RT_Event, b);
}

bool AuthorityLedger::is_resident(const EntityID& id) const { for (auto& r : residents_) if (r.id == id) return true; return false; }
bool AuthorityLedger::may_self_improve(const EntityID& id) const { for (auto& r : residents_) if (r.id == id) return r.self_improve; return false; }
const Resident* AuthorityLedger::resident(const std::string& label) const { for (auto& r : residents_) if (r.label == label) return &r; return nullptr; }

}  // namespace aios
