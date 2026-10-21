#pragma once
#include <cstdint>
#include <array>
#include <string>
#include "aios/core/bytes.hpp"

namespace aios::crypto {

struct SHA256Context {
  uint32_t state[8];
  uint64_t count;
  uint8_t buffer[64];
};

void sha256_init(SHA256Context& ctx);
void sha256_update(SHA256Context& ctx, const uint8_t* data, size_t len);
void sha256_final(SHA256Context& ctx, uint8_t hash[32]);

std::array<uint8_t, 32> sha256(ByteSpan data);
std::array<uint8_t, 32> sha256d(ByteSpan data);

std::string bytes_to_hex(ByteSpan data);
ByteBuffer hex_to_bytes(const std::string& hex);

}  // namespace aios::crypto
