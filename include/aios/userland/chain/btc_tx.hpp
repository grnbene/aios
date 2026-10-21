#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"

namespace aios::userland::chain {

enum class Network { Mainnet, Testnet, Signet, Regtest };

struct NetworkParams {
  uint8_t p2pkh_version;
  uint8_t p2sh_version;
  uint8_t wif_version;
  const char* bech32_hrp;
  uint32_t magic;
  uint16_t port;
  const char* name;
};

const NetworkParams& network_params(Network n);
Result<Network> network_by_name(const std::string& name);

struct OutPoint {
  std::array<uint8_t, 32> txid{};   // consensus byte order, not the reversed display form
  uint32_t index = 0;
};

struct TxIn {
  OutPoint prevout;
  ByteBuffer script_sig;
  uint32_t sequence = 0xfffffffdu;   // opts in to replace-by-fee and to locktime
  std::vector<ByteBuffer> witness;
};

struct TxOut {
  uint64_t value = 0;
  ByteBuffer script_pubkey;
};

struct Transaction {
  int32_t version = 2;
  std::vector<TxIn> inputs;
  std::vector<TxOut> outputs;
  uint32_t lock_time = 0;
  bool has_witness() const;
};

ByteBuffer serialize_tx(const Transaction& tx, bool with_witness);
Result<Transaction> deserialize_tx(ByteSpan raw);

std::array<uint8_t, 32> txid_of(const Transaction& tx);
std::array<uint8_t, 32> wtxid_of(const Transaction& tx);
std::string hash_to_display(const std::array<uint8_t, 32>& h);
Result<std::array<uint8_t, 32>> hash_from_display(const std::string& hex);

void write_varint(ByteBuffer& out, uint64_t value);
void write_push(ByteBuffer& out, ByteSpan data);

ByteBuffer script_p2pkh(const uint8_t hash160[20]);
ByteBuffer script_p2sh(const uint8_t hash160[20]);
ByteBuffer script_p2wpkh(const uint8_t hash160[20]);
ByteBuffer script_p2wsh(const uint8_t sha[32]);
ByteBuffer script_op_return(ByteSpan data);

// A hash-locked contract that also requires a signature, which is the shape the
// solver's HTLC talk has always implied and never actually produced.
ByteBuffer script_hashlock(const uint8_t sha256_target[32], ByteSpan redeem_pubkey);
ByteBuffer script_htlc(const uint8_t sha256_target[32], ByteSpan claim_pubkey, ByteSpan refund_pubkey,
                       uint32_t lock_time);

Result<ByteBuffer> script_for_address(const std::string& address, Network net);
Result<std::string> address_for_script(ByteSpan script, Network net);
Result<std::string> address_p2wpkh(ByteSpan pubkey, Network net);
Result<std::string> address_p2pkh(ByteSpan pubkey, Network net);

enum : uint8_t {
  SIGHASH_ALL = 0x01,
  SIGHASH_NONE = 0x02,
  SIGHASH_SINGLE = 0x03,
  SIGHASH_ANYONECANPAY = 0x80,
};

std::array<uint8_t, 32> sighash_legacy(const Transaction& tx, size_t index, ByteSpan script_code, uint8_t type);
std::array<uint8_t, 32> sighash_witness_v0(const Transaction& tx, size_t index, ByteSpan script_code,
                                           uint64_t amount, uint8_t type);

struct SpendableOutput {
  OutPoint prevout;
  uint64_t amount = 0;
  ByteBuffer script_pubkey;
  bool witness = true;
};

Result<void> sign_input_p2wpkh(Transaction& tx, size_t index, const uint8_t sec[32], uint64_t amount,
                               uint8_t type = SIGHASH_ALL);
Result<void> sign_input_p2pkh(Transaction& tx, size_t index, const uint8_t sec[32], uint8_t type = SIGHASH_ALL);

// Virtual size in vbytes: witness data counts a quarter, which is what every
// fee estimate on the network is quoted against.
size_t vsize_of(const Transaction& tx);

struct PaymentPlan {
  Transaction tx;
  uint64_t fee = 0;
  uint64_t change = 0;
  std::vector<SpendableOutput> spent;
};

Result<PaymentPlan> build_payment(const std::vector<SpendableOutput>& available, const std::string& recipient,
                                  uint64_t amount, const std::string& change_address, uint64_t sat_per_vbyte,
                                  Network net);

Result<void> sign_payment(PaymentPlan& plan, const uint8_t sec[32]);

// The script interpreter. Handles the subset the contracts here actually use:
// pushes, the hash and comparison opcodes, conditionals, CHECKSIG against a
// real transaction context, and CHECKLOCKTIMEVERIFY.
struct ScriptContext {
  const Transaction* tx = nullptr;
  size_t input_index = 0;
  uint64_t amount = 0;
  bool witness = false;
};

Result<bool> evaluate_script(ByteSpan script_sig_or_witness_stack, ByteSpan script_pubkey,
                             const ScriptContext& ctx);
Result<bool> evaluate_script_stack(const std::vector<ByteBuffer>& initial_stack, ByteSpan script,
                                   const ScriptContext& ctx);

}  // namespace aios::userland::chain
