#pragma once
// Curve25519 and Ed25519, implemented here because the machine has to do its
// own key agreement and its own signatures: the SSH transport is negotiated
// with curve25519-sha256 and the host key it checks is ssh-ed25519.
//
// Field arithmetic is radix 2^51 over GF(2^255-19); the group is the twisted
// Edwards curve in extended coordinates. Scalar multiplication selects with a
// conditional move rather than a branch, so the secret scalar does not steer
// control flow — but this is a software implementation without hardware
// side-channel hardening, and it is documented as such rather than claimed to
// be more than it is.
#include <array>
#include <cstdint>
#include "aios/core/bytes.hpp"

namespace aios::crypto {

using Scalar32 = std::array<uint8_t, 32>;
using Point32 = std::array<uint8_t, 32>;
using Sig64 = std::array<uint8_t, 64>;

// X25519 (RFC 7748). Returns false when the result is all zero (a small-order
// peer point), which the caller must treat as a failed key agreement.
bool x25519(Point32& out, const Scalar32& scalar, const Point32& peer);
// scalar * basepoint(9)
Point32 x25519_base(const Scalar32& scalar);

// Ed25519 (RFC 8032). The private key is the 32-byte seed.
Point32 ed25519_public_from_seed(const Scalar32& seed);
Sig64 ed25519_sign(ByteSpan message, const Scalar32& seed, const Point32& pub);
bool ed25519_verify(ByteSpan message, const Sig64& sig, const Point32& pub);

}  // namespace aios::crypto
