#pragma once
#include <string>
#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"

namespace aios {

std::string base64_encode(ByteSpan data);
Result<ByteBuffer> base64_decode(const std::string& text);

}  // namespace aios
