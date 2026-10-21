#pragma once
// Identity is immutable, 128-bit, and survives execution, checkpoint, restart
// and migration. Nothing in the OS is addressed by a pointer, a PID or a path.
#include <cstdint>
#include <cstring>
#include <string>
#include <functional>

namespace aios {

struct Id128 {
  uint64_t hi = 0;
  uint64_t lo = 0;
  constexpr bool is_null() const { return hi == 0 && lo == 0; }
  constexpr bool operator==(const Id128& o) const { return hi == o.hi && lo == o.lo; }
  constexpr bool operator!=(const Id128& o) const { return !(*this == o); }
  constexpr bool operator<(const Id128& o) const { return hi < o.hi || (hi == o.hi && lo < o.lo); }
  std::string str() const;                 // 32 lowercase hex chars
  std::string short_str() const;           // first 12 hex chars, for humans
  static bool parse(const std::string& s, Id128& out);
};

struct Id128Hash {
  size_t operator()(const Id128& id) const noexcept {
    return std::hash<uint64_t>{}(id.hi ^ (id.lo * 0x9E3779B97F4A7C15ull));
  }
};

// Typed identities. They share representation but never mix at compile time.
template <int Tag>
struct TypedId : Id128 {
  constexpr TypedId() = default;
  constexpr explicit TypedId(Id128 raw) : Id128(raw) {}
  static constexpr int tag = Tag;
};
template <int Tag> struct TypedIdHash { size_t operator()(const TypedId<Tag>& id) const noexcept { return Id128Hash{}(id); } };

using EntityID     = TypedId<1>;
using MemoryID     = TypedId<2>;
using StateID      = TypedId<3>;
using ChannelID    = TypedId<4>;
using CapabilityID = TypedId<5>;
using DeviceID     = TypedId<6>;
using GraphID      = TypedId<7>;
using OperationID  = TypedId<8>;
using ObjectID     = TypedId<9>;

// Produces identities that are unique across boots without coordination:
// a per-boot random prefix plus a monotonic counter.
class IdGenerator {
 public:
  explicit IdGenerator(uint64_t boot_entropy, uint64_t boot_ns);
  Id128 next();
  template <typename T> T next_as() { return T{next()}; }
 private:
  uint64_t entropy_;
  uint64_t counter_;
};

}  // namespace aios
