#pragma once
// Authority is a first-class, persistent kernel object. Every capability held
// by a RESIDENT (an entity the kernel keeps across boots: itself, a console, a
// storage service...) is recorded in an append-only ledger under the persistent
// root, together with every decision the kernel made about authority: what was
// minted, what was derived from what, what was revoked, what was asked for and
// what the kernel refused. The ledger is replayed at boot, so a resident wakes
// up holding exactly what it held, no more — and a refusal is never lost.
#include <string>
#include <vector>
#include "aios/capability/capability.hpp"
#include "aios/hal/host.hpp"

namespace aios {

struct Resident {
  EntityID id;
  std::string label;
  uint64_t created_ns = 0;
  bool self_improve = false;   // may petition the machine to grow its own authority (governed)
};

struct AuthorityEvent {
  enum Kind : uint8_t { Minted = 1, Derived = 2, Revoked = 3, Requested = 4, Refused = 5, Admitted = 6, Attenuated = 7, Exercised = 8 };
  Kind kind = Minted;
  uint64_t ns = 0;
  EntityID actor;          // who acted (the kernel itself for mints)
  EntityID subject;        // who the authority concerns
  ResourceRef resource;
  uint16_t rights = 0;
  CapabilityID capability; // the capability issued/revoked, when one was
  std::string note;        // the kernel's own words, e.g. why it refused
};
const char* authority_event_name(AuthorityEvent::Kind k);

class AuthorityLedger {
 public:
  AuthorityLedger(hal::Host& host, std::string path);
  // Replays the log. After open(): residents(), capabilities() (live ones) and
  // events() reflect what persisted.
  Result<void> open();
  bool persisted() const { return persisted_; }

  void record_resident(const Resident& r);
  void record_capability(const Capability& c);      // a resident now holds c
  void record_revoked(const CapabilityID& id);
  void record_event(const AuthorityEvent& e);

  const std::vector<Resident>& residents() const { return residents_; }
  const std::vector<Capability>& capabilities() const { return caps_; }
  const std::vector<AuthorityEvent>& events() const { return events_; }
  bool is_resident(const EntityID& id) const;
  bool may_self_improve(const EntityID& id) const;   // a resident the machine lets petition for authority
  const Resident* resident(const std::string& label) const;

 private:
  void append(uint8_t type, const ByteBuffer& payload);
  hal::Host& host_;
  std::string path_;
  bool persisted_ = false;
  std::vector<Resident> residents_;
  std::vector<Capability> caps_;
  std::vector<AuthorityEvent> events_;
};

}  // namespace aios
