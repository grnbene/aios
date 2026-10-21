#pragma once

#include <cstdint>
#include "aios/hal/sync.hpp"
#include <string>
#include <array>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#if !defined(AIOS_WASI_NO_THREADS)  // (wasm without threads: aios/core/wasi_threads.hpp declares the lock types)
#include <shared_mutex>
#endif
#include <optional>

namespace aios::sidecar {

enum class CapabilityRight : uint32_t {
  NONE      = 0,
  READ      = 1 << 0,
  WRITE     = 1 << 1,
  EXECUTE   = 1 << 2,
  DELETE_OP = 1 << 3,
  DELEGATE  = 1 << 4,
  ADMIN     = 1 << 5,
  ALL       = 0x3F,

  // PascalCase aliases for ergonomic multi-agent API compatibility
  None      = NONE,
  Read      = READ,
  Write     = WRITE,
  Execute   = EXECUTE,
  DeleteOp  = DELETE_OP,
  Delegate  = DELEGATE,
  Admin     = ADMIN,
  All       = ALL
};

inline CapabilityRight operator|(CapabilityRight a, CapabilityRight b) {
  return static_cast<CapabilityRight>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline CapabilityRight operator&(CapabilityRight a, CapabilityRight b) {
  return static_cast<CapabilityRight>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool has_right(CapabilityRight mask, CapabilityRight right) {
  return (static_cast<uint32_t>(mask) & static_cast<uint32_t>(right)) == static_cast<uint32_t>(right);
}

struct alignas(16) CapabilityToken {
  uint64_t object_id_hi{0};
  uint64_t object_id_lo{0};
  uint32_t rights{0};
  uint32_t generation{0};
  uint64_t expires_at_ns{0};
  uint8_t mac[16]{0}; // HMAC-SHA256 truncated MAC
};

static_assert(sizeof(CapabilityToken) == 48, "CapabilityToken must be 48 bytes");

class CapabilityGuard {
public:
  // The key every token is signed with. There is no default: a key everyone
  // with the source knows signs nothing. The engine loads it per machine
  // (AIOS_CAPABILITY_KEY, else <root>/capability.key, made on first start).
  //
  // `state_dir` is where revocations are kept so they outlive a restart; empty
  // keeps them in memory only. Give one only with a key that itself survives a
  // restart - a guard on a random per-process key has no tokens to revoke
  // afterwards, and guards on different keys must not share a directory.
  explicit CapabilityGuard(const std::string& master_secret, const std::string& state_dir = "");
  ~CapabilityGuard() = default;

  // Issue a new capability token for a target object namespace
  CapabilityToken mint_token(uint64_t obj_hi, uint64_t obj_lo,
                             CapabilityRight rights,
                             uint64_t ttl_ns = 0);

  // Mint token for a string resource name (e.g. "table:hairlines.dispatch")
  CapabilityToken mint_for_resource(const std::string& resource_name,
                                    CapabilityRight rights,
                                    uint64_t ttl_ns = 0);

  // The token this guard's key signs for exactly these fields. A verifier
  // rebuilds the root of a credential that carries its fields but not its MAC.
  CapabilityToken token_for_fields(const std::string& resource_name, uint32_t rights,
                                   uint32_t generation, uint64_t expires_at_ns) const;

  // Validate a capability token
  bool validate_token(const CapabilityToken& token,
                      uint64_t obj_hi, uint64_t obj_lo,
                      CapabilityRight required_right) const;

  bool validate_for_resource(const CapabilityToken& token,
                             const std::string& resource_name,
                             CapabilityRight required_right) const;

  // Attenuate an existing valid token (derive a sub-token with lesser rights or shorter TTL)
  std::optional<CapabilityToken> attenuate(const CapabilityToken& parent_token,
                                           CapabilityRight reduced_rights,
                                           uint64_t shorter_ttl_ns = 0);

  // Revoke an object namespace: bumps its generation, and writes the store when
  // there is one. False means the store could not be written - the revocation
  // holds in this process but would be lost on restart.
  bool revoke_resource(const std::string& resource_name);

  // False when a store exists but is damaged or was not written by this key.
  // The revocations it held are unknown, so every resource check then fails.
  bool revocation_store_ok() const { return store_ok_; }

private:
  std::string secret_;
  mutable aios::hal::SharedMutex mutex_;
  std::unordered_map<std::string, uint32_t> resource_generations_;
  std::string generations_file_;
  bool store_ok_{true};

  void sign_token(CapabilityToken& token) const;
  bool verify_signature(const CapabilityToken& token) const;
  static std::pair<uint64_t, uint64_t> hash_resource_name(const std::string& name);

  std::string encode_generations() const;
  bool decode_generations(const std::string& content, std::unordered_map<std::string, uint32_t>* out) const;
  void load_generations();
  bool save_generations() const;
};

} // namespace aios::sidecar
