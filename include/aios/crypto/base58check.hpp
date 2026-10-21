#pragma once
#include <string>
#include <cstdint>
#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"

namespace aios::crypto {

std::string base58_encode(ByteSpan data);
Result<ByteBuffer> base58_decode(const std::string& text);

std::string base58check_encode(uint8_t version, ByteSpan payload);
Result<ByteBuffer> base58check_decode(const std::string& text, uint8_t& version);

}  // namespace aios::crypto
