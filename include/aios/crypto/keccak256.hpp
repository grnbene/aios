#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include "aios/core/bytes.hpp"

namespace aios::crypto {

// Original Keccak padding (0x01), not the SHA-3 padding (0x06). Ethereum uses
// this one; substituting SHA-3 produces addresses that look plausible and are
// wrong.
std::array<uint8_t, 32> keccak256(ByteSpan data);

}  // namespace aios::crypto
