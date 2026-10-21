#include "aios/userland/chain/eth_tx.hpp"
#include <cstring>
#include "aios/crypto/keccak256.hpp"
#include "aios/crypto/secp256k1.hpp"
#include "aios/userland/chain/rlp.hpp"

namespace aios::userland::chain {

using namespace aios::crypto;

namespace {

const ByteBuffer EMPTY_LIST = {0xc0};

ByteBuffer encoded_access_list(const EthTransaction& tx) {
  return tx.access_list.empty() ? EMPTY_LIST : tx.access_list;
}

ByteBuffer encoded_recipient(const EthTransaction& tx) {
  if (tx.creates_contract) return rlp_bytes(ByteSpan());
  return rlp_bytes(ByteSpan(tx.to.data(), tx.to.size()));
}

std::vector<ByteBuffer> body_1559(const EthTransaction& tx) {
  return {rlp_number(tx.chain_id),
          rlp_number(tx.nonce),
          rlp_quantity(ByteSpan(tx.max_priority_fee_per_gas.data(), tx.max_priority_fee_per_gas.size())),
          rlp_quantity(ByteSpan(tx.max_fee_per_gas.data(), tx.max_fee_per_gas.size())),
          rlp_number(tx.gas_limit),
          encoded_recipient(tx),
          rlp_quantity(ByteSpan(tx.value.data(), tx.value.size())),
          rlp_bytes(ByteSpan(tx.data.data(), tx.data.size())),
          encoded_access_list(tx)};
}

std::vector<ByteBuffer> body_legacy(const EthTransaction& tx) {
  return {rlp_number(tx.nonce),
          rlp_quantity(ByteSpan(tx.gas_price.data(), tx.gas_price.size())),
          rlp_number(tx.gas_limit),
          encoded_recipient(tx),
          rlp_quantity(ByteSpan(tx.value.data(), tx.value.size())),
          rlp_bytes(ByteSpan(tx.data.data(), tx.data.size()))};
}

ByteBuffer trimmed(const std::array<uint8_t, 32>& v) {
  size_t start = 0;
  while (start < 32 && v[start] == 0) ++start;
  return ByteBuffer(v.begin() + (ptrdiff_t)start, v.end());
}

int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

ByteBuffer eth_signing_payload_1559(const EthTransaction& tx) {
  ByteBuffer out;
  out.push_back(0x02);
  ByteBuffer body = rlp_list(body_1559(tx));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

ByteBuffer eth_signing_payload_legacy(const EthTransaction& tx) {
  auto fields = body_legacy(tx);
  fields.push_back(rlp_number(tx.chain_id));
  fields.push_back(rlp_bytes(ByteSpan()));
  fields.push_back(rlp_bytes(ByteSpan()));
  return rlp_list(fields);
}

ByteBuffer eth_encode_signed_1559(const EthTransaction& tx, int y_parity, ByteSpan r, ByteSpan s) {
  auto fields = body_1559(tx);
  fields.push_back(rlp_number((uint64_t)(y_parity & 1)));
  fields.push_back(rlp_quantity(r));
  fields.push_back(rlp_quantity(s));

  ByteBuffer out;
  out.push_back(0x02);
  ByteBuffer body = rlp_list(fields);
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

Result<ByteBuffer> eth_sign_1559(const EthTransaction& tx, const uint8_t sec[32]) {
  auto payload = eth_signing_payload_1559(tx);
  auto digest = keccak256(ByteSpan(payload.data(), payload.size()));
  auto sig = ecdsa_sign(sec, digest.data());
  if (!sig.ok()) return Result<ByteBuffer>(sig.error());

  ByteBuffer r = trimmed(sig->r);
  ByteBuffer s = trimmed(sig->s);
  return eth_encode_signed_1559(tx, sig->recovery_id & 1, ByteSpan(r.data(), r.size()),
                                ByteSpan(s.data(), s.size()));
}

Result<ByteBuffer> eth_sign_legacy(const EthTransaction& tx, const uint8_t sec[32]) {
  auto payload = eth_signing_payload_legacy(tx);
  auto digest = keccak256(ByteSpan(payload.data(), payload.size()));
  auto sig = ecdsa_sign(sec, digest.data());
  if (!sig.ok()) return Result<ByteBuffer>(sig.error());

  auto fields = body_legacy(tx);
  uint64_t v = (uint64_t)(sig->recovery_id & 1) + tx.chain_id * 2 + 35;
  fields.push_back(rlp_number(v));
  ByteBuffer r = trimmed(sig->r);
  ByteBuffer s = trimmed(sig->s);
  fields.push_back(rlp_bytes(ByteSpan(r.data(), r.size())));
  fields.push_back(rlp_bytes(ByteSpan(s.data(), s.size())));
  return rlp_list(fields);
}

std::array<uint8_t, 32> eth_transaction_hash(ByteSpan raw) { return keccak256(raw); }

Result<EthAddress> eth_recover_sender(ByteSpan raw) {
  if (raw.empty()) return Result<EthAddress>(Status::Invalid, "empty transaction");

  bool typed = raw[0] <= 0x7f;
  uint8_t type = typed ? raw[0] : 0;
  ByteSpan body = typed ? raw.subspan(1) : raw;

  auto decoded = rlp_decode(body);
  if (!decoded.ok()) return Result<EthAddress>(decoded.error());
  if (!decoded->is_list) return Result<EthAddress>(Status::Invalid, "transaction is not an rlp list");

  const auto& items = decoded->items;
  EcSignature sig;
  ByteBuffer signing_payload;

  if (typed && (type == 0x02 || type == 0x01)) {
    size_t expected = (type == 0x02) ? 12u : 11u;
    if (items.size() != expected) return Result<EthAddress>(Status::Invalid, "typed transaction field count");
    std::vector<ByteBuffer> fields;
    for (size_t i = 0; i + 3 < items.size(); ++i) {
      fields.push_back(items[i].is_list ? rlp_list({}) : rlp_bytes(ByteSpan(items[i].data.data(), items[i].data.size())));
    }
    // The access list is a list of lists; re-encoding it needs the original
    // bytes, which the decoder does not keep, so it is rebuilt from its items.
    if (items[items.size() - 4].is_list && items[items.size() - 4].items.empty()) {
      fields[fields.size() - 1] = ByteBuffer{0xc0};
    } else if (items[items.size() - 4].is_list) {
      return Result<EthAddress>(Status::Unsupported, "non-empty access list is not re-encoded here");
    }

    signing_payload.push_back(type);
    ByteBuffer encoded = rlp_list(fields);
    signing_payload.insert(signing_payload.end(), encoded.begin(), encoded.end());

    uint64_t parity = items[items.size() - 3].as_u64();
    sig.recovery_id = (int)(parity & 1);
    const auto& r = items[items.size() - 2].data;
    const auto& s = items[items.size() - 1].data;
    if (r.size() > 32 || s.size() > 32) return Result<EthAddress>(Status::Invalid, "signature component too wide");
    std::memcpy(sig.r.data() + (32 - r.size()), r.data(), r.size());
    std::memcpy(sig.s.data() + (32 - s.size()), s.data(), s.size());
  } else if (!typed) {
    if (items.size() != 9) return Result<EthAddress>(Status::Invalid, "legacy transaction field count");
    uint64_t v = items[6].as_u64();
    uint64_t chain_id = 0;
    int parity = 0;
    if (v >= 35) {
      chain_id = (v - 35) / 2;
      parity = (int)((v - 35) % 2);
    } else if (v == 27 || v == 28) {
      parity = (int)(v - 27);
    } else {
      return Result<EthAddress>(Status::Invalid, "legacy v out of range");
    }

    std::vector<ByteBuffer> fields;
    for (size_t i = 0; i < 6; ++i) {
      fields.push_back(rlp_bytes(ByteSpan(items[i].data.data(), items[i].data.size())));
    }
    if (v >= 35) {
      fields.push_back(rlp_number(chain_id));
      fields.push_back(rlp_bytes(ByteSpan()));
      fields.push_back(rlp_bytes(ByteSpan()));
    }
    signing_payload = rlp_list(fields);

    sig.recovery_id = parity;
    const auto& r = items[7].data;
    const auto& s = items[8].data;
    if (r.size() > 32 || s.size() > 32) return Result<EthAddress>(Status::Invalid, "signature component too wide");
    std::memcpy(sig.r.data() + (32 - r.size()), r.data(), r.size());
    std::memcpy(sig.s.data() + (32 - s.size()), s.data(), s.size());
  } else {
    return Result<EthAddress>(Status::Unsupported, "unsupported transaction type");
  }

  auto digest = keccak256(ByteSpan(signing_payload.data(), signing_payload.size()));
  auto pubkey = ecdsa_recover(sig, digest.data(), false);
  if (!pubkey.ok()) return Result<EthAddress>(pubkey.error());
  auto address = eth_address_from_pubkey(ByteSpan(pubkey->data(), pubkey->size()));
  if (!address.ok()) return Result<EthAddress>(address.error());
  return *address;
}

std::string eth_checksum_address(const EthAddress& address) {
  static const char* HEX = "0123456789abcdef";
  std::string lower;
  for (uint8_t b : address) {
    lower.push_back(HEX[b >> 4]);
    lower.push_back(HEX[b & 15]);
  }
  auto digest = keccak256(ByteSpan(reinterpret_cast<const uint8_t*>(lower.data()), lower.size()));

  std::string out = "0x";
  for (size_t i = 0; i < lower.size(); ++i) {
    uint8_t nibble = (i % 2 == 0) ? (uint8_t)(digest[i / 2] >> 4) : (uint8_t)(digest[i / 2] & 15);
    char c = lower[i];
    if (c >= 'a' && c <= 'f' && nibble >= 8) c = (char)(c - 32);
    out.push_back(c);
  }
  return out;
}

Result<EthAddress> eth_parse_address(const std::string& text) {
  std::string body = text;
  if (body.size() > 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X')) body = body.substr(2);
  if (body.size() != 40) return Result<EthAddress>(Status::Invalid, "an address is 20 bytes");

  EthAddress out{};
  for (size_t i = 0; i < 20; ++i) {
    int hi = hex_nibble(body[i * 2]);
    int lo = hex_nibble(body[i * 2 + 1]);
    if (hi < 0 || lo < 0) return Result<EthAddress>(Status::Invalid, "address is not hex");
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return out;
}

Result<EthAddress> eth_address_of_key(const uint8_t sec[32]) {
  auto pubkey = ec_pubkey_create(sec, false);
  if (!pubkey.ok()) return Result<EthAddress>(pubkey.error());
  return eth_address_from_pubkey(ByteSpan(pubkey->data(), pubkey->size()));
}

ByteBuffer eth_quantity_from_hex(const std::string& text) {
  std::string body = text;
  if (body.size() > 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X')) body = body.substr(2);
  if (body.size() % 2) body.insert(body.begin(), '0');

  ByteBuffer out;
  for (size_t i = 0; i + 1 < body.size(); i += 2) {
    int hi = hex_nibble(body[i]);
    int lo = hex_nibble(body[i + 1]);
    if (hi < 0 || lo < 0) return ByteBuffer();
    out.push_back((uint8_t)((hi << 4) | lo));
  }
  size_t start = 0;
  while (start + 1 < out.size() && out[start] == 0) ++start;
  return ByteBuffer(out.begin() + (ptrdiff_t)start, out.end());
}

std::string eth_hex_of(ByteSpan data) {
  static const char* HEX = "0123456789abcdef";
  std::string out = "0x";
  if (data.empty()) return "0x0";
  for (uint8_t b : data) {
    out.push_back(HEX[b >> 4]);
    out.push_back(HEX[b & 15]);
  }
  return out;
}

}  // namespace aios::userland::chain
