#pragma once
#include <cstdint>
#include <vector>
#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"

namespace aios::userland::chain {

// Recursive Length Prefix, the only encoding Ethereum signs over. Quantities
// carry no leading zeros and zero itself is the empty string, which is the
// detail that silently produces a different transaction hash when missed.
ByteBuffer rlp_bytes(ByteSpan data);
ByteBuffer rlp_quantity(ByteSpan big_endian);
ByteBuffer rlp_number(uint64_t value);
ByteBuffer rlp_list(const std::vector<ByteBuffer>& encoded_items);

struct RlpNode {
  bool is_list = false;
  ByteBuffer data;
  std::vector<RlpNode> items;
  uint64_t as_u64() const;
};

Result<RlpNode> rlp_decode(ByteSpan raw);

}  // namespace aios::userland::chain
