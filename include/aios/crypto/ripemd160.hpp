#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include "aios/core/bytes.hpp"

namespace aios::crypto {

std::array<uint8_t, 20> ripemd160(ByteSpan data);

// The Bitcoin address digest: RIPEMD160(SHA256(x)).
std::array<uint8_t, 20> hash160(ByteSpan data);

}  // namespace aios::crypto
