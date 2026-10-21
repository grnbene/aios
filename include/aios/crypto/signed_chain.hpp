#pragma once
// Signed hash chains: a service's own Ed25519 key, and records that each name the hash of the one
// before, so anyone holding the public key can check from the records alone that every one was
// signed by that key and that none was dropped, inserted or reordered.
//
// A record is a JSON object whose bytes are signed exactly as they are; it must carry "seq"
// (1, 2, 3...) and "prev" (lowercase hex sha256 of the previous record's bytes; "" for seq 1).
// Everything else in it is the caller's. The record travels with its signature (hex) and is never
// re-serialised: the bytes signed are the bytes checked.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "aios/core/result.hpp"
#include "aios/crypto/curve25519.hpp"

namespace aios::crypto {

class Ed25519Signer {
 public:
  // Loads the 32-byte seed from `path`, creating it (mode 0600, from /dev/urandom) on first use.
  Result<void> open(const std::string& path);
  // A seed already in memory (from a secret store or the environment); no key file involved.
  Result<void> open_seed(const Scalar32& seed);
  // Exactly 64 hex characters into a 32-byte seed.
  static Result<Scalar32> seed_from_hex(std::string_view hex);
  bool ready() const { return ready_; }
  std::string public_hex() const;
  std::string sign_hex(std::string_view message) const;

 private:
  bool ready_ = false;
  Scalar32 seed_{};
  Point32 pub_{};
};

std::string hex_lower(const uint8_t* p, size_t n);
std::string sha256_hex(std::string_view data);
// False on a malformed key or signature as well as a wrong one.
bool ed25519_verify_hex(std::string_view message, std::string_view signature_hex, std::string_view public_hex);

struct ChainLink {
  std::string record;      // the signed bytes
  std::string signature;   // hex
};

struct ChainCheck {
  size_t links = 0;
  std::vector<std::string> problems;  // empty: every signature verifies and the chain is unbroken
  bool ok() const { return problems.empty(); }
};

// Checks links in order against one public key: each signature, seq = position + first_seq, and
// prev = sha256 of the previous record (for the first link, `first_prev`).
ChainCheck verify_chain(const std::vector<ChainLink>& links, std::string_view public_hex,
                        uint64_t first_seq = 1, std::string_view first_prev = "");

}  // namespace aios::crypto
