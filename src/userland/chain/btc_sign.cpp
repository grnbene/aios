#include <algorithm>
#include <cstring>
#include "aios/crypto/ripemd160.hpp"
#include "aios/crypto/secp256k1.hpp"
#include "aios/userland/chain/btc_tx.hpp"

namespace aios::userland::chain {

using namespace aios::crypto;

namespace {

const uint64_t DUST_LIMIT = 546;

ByteBuffer der_with_type(const EcSignature& sig, uint8_t type) {
  ByteBuffer out = ecdsa_der_encode(sig);
  out.push_back(type);
  return out;
}

// A signed input's real size is not known until it is signed, and the fee
// depends on the size. Filling the witness with items of the right length lets
// vsize_of measure the transaction that will exist rather than guessing at it.
void fill_placeholder_witness(Transaction& tx) {
  for (auto& in : tx.inputs) {
    if (!in.witness.empty()) continue;
    in.witness.push_back(ByteBuffer(72, 0x30));
    in.witness.push_back(ByteBuffer(33, 0x02));
  }
}

void clear_placeholder_witness(Transaction& tx) {
  for (auto& in : tx.inputs) in.witness.clear();
}

}  // namespace

Result<void> sign_input_p2wpkh(Transaction& tx, size_t index, const uint8_t sec[32], uint64_t amount,
                               uint8_t type) {
  if (index >= tx.inputs.size()) return Result<void>(Status::Invalid, "input index out of range");

  auto pubkey = ec_pubkey_create(sec, true);
  if (!pubkey.ok()) return Result<void>(pubkey.error());

  auto h = hash160(ByteSpan(pubkey->data(), pubkey->size()));
  ByteBuffer script_code = script_p2pkh(h.data());

  auto digest = sighash_witness_v0(tx, index, ByteSpan(script_code.data(), script_code.size()), amount, type);
  auto sig = ecdsa_sign(sec, digest.data());
  if (!sig.ok()) return Result<void>(sig.error());

  tx.inputs[index].script_sig.clear();
  tx.inputs[index].witness.clear();
  tx.inputs[index].witness.push_back(der_with_type(*sig, type));
  tx.inputs[index].witness.push_back(*pubkey);
  return Ok();
}

Result<void> sign_input_p2pkh(Transaction& tx, size_t index, const uint8_t sec[32], uint8_t type) {
  if (index >= tx.inputs.size()) return Result<void>(Status::Invalid, "input index out of range");

  auto pubkey = ec_pubkey_create(sec, true);
  if (!pubkey.ok()) return Result<void>(pubkey.error());

  auto h = hash160(ByteSpan(pubkey->data(), pubkey->size()));
  ByteBuffer script_code = script_p2pkh(h.data());

  auto digest = sighash_legacy(tx, index, ByteSpan(script_code.data(), script_code.size()), type);
  auto sig = ecdsa_sign(sec, digest.data());
  if (!sig.ok()) return Result<void>(sig.error());

  ByteBuffer script_sig;
  ByteBuffer signature = der_with_type(*sig, type);
  write_push(script_sig, ByteSpan(signature.data(), signature.size()));
  write_push(script_sig, ByteSpan(pubkey->data(), pubkey->size()));

  tx.inputs[index].script_sig = script_sig;
  tx.inputs[index].witness.clear();
  return Ok();
}

Result<PaymentPlan> build_payment(const std::vector<SpendableOutput>& available, const std::string& recipient,
                                  uint64_t amount, const std::string& change_address, uint64_t sat_per_vbyte,
                                  Network net) {
  if (amount == 0) return Result<PaymentPlan>(Status::Invalid, "amount must be positive");
  if (sat_per_vbyte == 0) return Result<PaymentPlan>(Status::Invalid, "fee rate must be positive");

  auto recipient_script = script_for_address(recipient, net);
  if (!recipient_script.ok()) return Result<PaymentPlan>(recipient_script.error());
  auto change_script = script_for_address(change_address, net);
  if (!change_script.ok()) return Result<PaymentPlan>(change_script.error());

  std::vector<SpendableOutput> sorted = available;
  std::sort(sorted.begin(), sorted.end(),
            [](const SpendableOutput& a, const SpendableOutput& b) { return a.amount > b.amount; });

  PaymentPlan plan;
  plan.tx.version = 2;
  plan.tx.outputs.push_back(TxOut{amount, *recipient_script});

  uint64_t collected = 0;
  for (const auto& utxo : sorted) {
    TxIn in;
    in.prevout = utxo.prevout;
    plan.tx.inputs.push_back(in);
    plan.spent.push_back(utxo);
    collected += utxo.amount;

    Transaction probe = plan.tx;
    probe.outputs.push_back(TxOut{0, *change_script});
    fill_placeholder_witness(probe);
    uint64_t fee = (uint64_t)vsize_of(probe) * sat_per_vbyte;

    if (collected < amount + fee) continue;

    uint64_t change = collected - amount - fee;
    if (change > DUST_LIMIT) {
      plan.tx.outputs.push_back(TxOut{change, *change_script});
      plan.change = change;
      plan.fee = fee;
    } else {
      Transaction no_change = plan.tx;
      fill_placeholder_witness(no_change);
      uint64_t fee_no_change = (uint64_t)vsize_of(no_change) * sat_per_vbyte;
      if (collected < amount + fee_no_change) continue;
      plan.change = 0;
      plan.fee = collected - amount;
    }
    clear_placeholder_witness(plan.tx);
    return plan;
  }

  return Result<PaymentPlan>(Status::Exhausted, "not enough confirmed value to cover amount plus fee");
}

Result<void> sign_payment(PaymentPlan& plan, const uint8_t sec[32]) {
  if (plan.spent.size() != plan.tx.inputs.size()) {
    return Result<void>(Status::Invalid, "plan inputs and spent outputs disagree");
  }
  for (size_t i = 0; i < plan.tx.inputs.size(); ++i) {
    Result<void> signed_ok = plan.spent[i].witness
                                 ? sign_input_p2wpkh(plan.tx, i, sec, plan.spent[i].amount)
                                 : sign_input_p2pkh(plan.tx, i, sec);
    if (!signed_ok.ok()) return signed_ok;
  }
  return Ok();
}

}  // namespace aios::userland::chain
