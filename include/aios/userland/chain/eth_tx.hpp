#pragma once
#include <array>
#include <cstdint>
#include <string>
#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"

namespace aios::userland::chain {

using EthAddress = std::array<uint8_t, 20>;

struct EthTransaction {
  uint64_t chain_id = 1;
  uint64_t nonce = 0;
  uint64_t gas_limit = 21000;
  EthAddress to{};
  bool creates_contract = false;

  ByteBuffer value;                  // wei, big-endian, no width limit
  ByteBuffer data;
  ByteBuffer gas_price;              // legacy only
  ByteBuffer max_fee_per_gas;        // type 2
  ByteBuffer max_priority_fee_per_gas;
  ByteBuffer access_list;            // pre-encoded rlp; empty means the empty list
};

// The bytes that get hashed and signed. Type 2 is what nodes want now; the
// legacy form exists because the EIP-155 vector is written against it.
ByteBuffer eth_signing_payload_1559(const EthTransaction& tx);
ByteBuffer eth_signing_payload_legacy(const EthTransaction& tx);

Result<ByteBuffer> eth_sign_1559(const EthTransaction& tx, const uint8_t sec[32]);
Result<ByteBuffer> eth_sign_legacy(const EthTransaction& tx, const uint8_t sec[32]);

// Assembles a type 2 transaction around a signature produced elsewhere, which
// is what lets a transaction the network already carried be rebuilt here and
// compared against its canonical hash.
ByteBuffer eth_encode_signed_1559(const EthTransaction& tx, int y_parity, ByteSpan r, ByteSpan s);

std::array<uint8_t, 32> eth_transaction_hash(ByteSpan raw);

// Recovers the sender of an already-signed transaction, which is how a signing
// implementation is checked against transactions the network itself accepted.
Result<EthAddress> eth_recover_sender(ByteSpan raw);

std::string eth_checksum_address(const EthAddress& address);
Result<EthAddress> eth_parse_address(const std::string& text);
Result<EthAddress> eth_address_of_key(const uint8_t sec[32]);

ByteBuffer eth_quantity_from_hex(const std::string& text);
std::string eth_hex_of(ByteSpan data);

}  // namespace aios::userland::chain
