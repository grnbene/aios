#include "aios/userland/chain/rlp.hpp"

namespace aios::userland::chain {

namespace {

ByteBuffer minimal_be(uint64_t value) {
  ByteBuffer out;
  bool started = false;
  for (int shift = 56; shift >= 0; shift -= 8) {
    uint8_t b = (uint8_t)(value >> shift);
    if (b == 0 && !started) continue;
    started = true;
    out.push_back(b);
  }
  return out;
}

void push_length(ByteBuffer& out, size_t length, uint8_t short_base, uint8_t long_base) {
  if (length <= 55) {
    out.push_back((uint8_t)(short_base + length));
    return;
  }
  ByteBuffer encoded = minimal_be((uint64_t)length);
  out.push_back((uint8_t)(long_base + encoded.size()));
  out.insert(out.end(), encoded.begin(), encoded.end());
}

Result<RlpNode> decode_at(ByteSpan raw, size_t& pos, int depth) {
  if (depth > 32) return Result<RlpNode>(Status::Invalid, "rlp nesting too deep");
  if (pos >= raw.size()) return Result<RlpNode>(Status::Invalid, "rlp ended early");

  uint8_t prefix = raw[pos];
  RlpNode node;

  auto read_length = [&](size_t bytes) -> Result<size_t> {
    if (pos + bytes > raw.size()) return Result<size_t>(Status::Invalid, "rlp length runs past the end");
    size_t length = 0;
    for (size_t i = 0; i < bytes; ++i) length = (length << 8) | raw[pos + i];
    pos += bytes;
    return length;
  };

  if (prefix <= 0x7f) {
    node.data.push_back(prefix);
    ++pos;
    return node;
  }

  if (prefix <= 0xb7) {
    size_t length = (size_t)(prefix - 0x80);
    ++pos;
    if (pos + length > raw.size()) return Result<RlpNode>(Status::Invalid, "rlp string runs past the end");
    node.data.assign(raw.begin() + (ptrdiff_t)pos, raw.begin() + (ptrdiff_t)(pos + length));
    pos += length;
    return node;
  }

  if (prefix <= 0xbf) {
    size_t header = (size_t)(prefix - 0xb7);
    ++pos;
    auto length = read_length(header);
    if (!length.ok()) return Result<RlpNode>(length.error());
    if (pos + *length > raw.size()) return Result<RlpNode>(Status::Invalid, "rlp string runs past the end");
    node.data.assign(raw.begin() + (ptrdiff_t)pos, raw.begin() + (ptrdiff_t)(pos + *length));
    pos += *length;
    return node;
  }

  node.is_list = true;
  size_t payload = 0;
  if (prefix <= 0xf7) {
    payload = (size_t)(prefix - 0xc0);
    ++pos;
  } else {
    size_t header = (size_t)(prefix - 0xf7);
    ++pos;
    auto length = read_length(header);
    if (!length.ok()) return Result<RlpNode>(length.error());
    payload = *length;
  }
  if (pos + payload > raw.size()) return Result<RlpNode>(Status::Invalid, "rlp list runs past the end");

  size_t end = pos + payload;
  while (pos < end) {
    auto item = decode_at(raw, pos, depth + 1);
    if (!item.ok()) return item;
    node.items.push_back(std::move(*item));
  }
  if (pos != end) return Result<RlpNode>(Status::Invalid, "rlp list overran its declared length");
  return node;
}

}  // namespace

ByteBuffer rlp_bytes(ByteSpan data) {
  ByteBuffer out;
  if (data.size() == 1 && data[0] <= 0x7f) {
    out.push_back(data[0]);
    return out;
  }
  push_length(out, data.size(), 0x80, 0xb7);
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

ByteBuffer rlp_quantity(ByteSpan big_endian) {
  size_t start = 0;
  while (start < big_endian.size() && big_endian[start] == 0) ++start;
  return rlp_bytes(big_endian.subspan(start));
}

ByteBuffer rlp_number(uint64_t value) {
  ByteBuffer be = minimal_be(value);
  return rlp_bytes(ByteSpan(be.data(), be.size()));
}

ByteBuffer rlp_list(const std::vector<ByteBuffer>& encoded_items) {
  size_t payload = 0;
  for (const auto& item : encoded_items) payload += item.size();

  ByteBuffer out;
  push_length(out, payload, 0xc0, 0xf7);
  for (const auto& item : encoded_items) out.insert(out.end(), item.begin(), item.end());
  return out;
}

uint64_t RlpNode::as_u64() const {
  uint64_t v = 0;
  for (uint8_t b : data) v = (v << 8) | b;
  return v;
}

Result<RlpNode> rlp_decode(ByteSpan raw) {
  size_t pos = 0;
  auto node = decode_at(raw, pos, 0);
  if (!node.ok()) return node;
  if (pos != raw.size()) return Result<RlpNode>(Status::Invalid, "trailing bytes after rlp item");
  return node;
}

}  // namespace aios::userland::chain
