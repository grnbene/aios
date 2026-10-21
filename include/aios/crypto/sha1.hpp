#pragma once
// SHA-1. Not a security primitive any more, and not used as one here: git
// names every object by the SHA-1 of its serialized form, so an OS that
// implements the git object format itself has to compute the same digest byte
// for byte. Freestanding, like the rest of aios::crypto — no host library —
// except that sha1_update uses the processor's SHA instructions when it has
// them (SHA-NI on x86-64, the crypto extensions on ARM64, detected at run
// time), with the same results as the portable code it falls back to.
#include <cstdint>
#include <array>
#include <string>
#include "aios/core/bytes.hpp"

namespace aios::crypto {

struct SHA1Context {
  uint32_t state[5];
  uint64_t count;      // message bytes consumed
  uint8_t buffer[64];
};

void sha1_init(SHA1Context& ctx);
void sha1_update(SHA1Context& ctx, const uint8_t* data, size_t len);
void sha1_final(SHA1Context& ctx, uint8_t hash[20]);

std::array<uint8_t, 20> sha1(ByteSpan data);

}  // namespace aios::crypto
