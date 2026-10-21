#pragma once
#include <string>
#include <cstdint>
#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"

namespace aios::crypto {

enum class Bech32Variant { Bech32, Bech32m };

std::string bech32_encode(const std::string& hrp, ByteSpan data5, Bech32Variant variant);
Result<ByteBuffer> bech32_decode(const std::string& text, std::string& hrp, Bech32Variant& variant);

// Segwit addresses: witness version plus the program, converted between 8-bit
// and 5-bit groups. Version 0 is bech32, versions 1..16 are bech32m.
std::string segwit_encode(const std::string& hrp, uint8_t version, ByteSpan program);
Result<ByteBuffer> segwit_decode(const std::string& text, const std::string& expected_hrp, uint8_t& version);

}  // namespace aios::crypto
