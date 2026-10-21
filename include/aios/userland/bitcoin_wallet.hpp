#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/userland/chain/btc_tx.hpp"
#include "aios/userland/chain/eth_tx.hpp"
#include "aios/userland/chain/payout.hpp"

namespace aios::userland {

struct UTXO {
  std::string txid;          // display order, as a node reports it
  uint32_t vout = 0;
  uint64_t amount_sats = 0;
  std::string script_pubkey;
  bool spent = false;
  std::string reserved_by;   // txid of a transaction signed here that spends it
};

struct BitcoinTransaction {
  std::string txid;
  std::string raw_hex;       // consensus bytes, ready for a peer
  uint64_t fee_sats = 0;
  uint64_t timestamp = 0;
  std::string recipient;
  uint64_t amount_sats = 0;
  uint64_t vsize = 0;
};

// A wallet that holds a key. The balance it reports is the value of outputs
// this key can spend, obtained from a node, and every transaction it produces
// is signed with that key and is the bytes a peer would relay.
class BitcoinWallet {
 public:
  explicit BitcoinWallet(Kernel& kernel);

  Result<void> init(const std::string& wallet_name = "default",
                    chain::Network network = chain::Network::Signet);
  Result<void> generate_new_keys();
  Result<void> import_wif(const std::string& wif);
  Result<void> load_state();
  Result<void> save_state();

  // The passphrase the private key is encrypted under at rest. Without one the
  // wallet keeps public material only and says so rather than writing a
  // spendable secret to the filesystem in the clear.
  void unlock(const std::string& passphrase);
  // True only when a passphrase was given AND (there was no sealed key on file,
  // or it opened the sealed key). A passphrase that does not open the key on
  // file is not "unlocked".
  bool unlocked() const { return !passphrase_.empty() && unlock_error_.empty(); }
  bool has_key() const { return has_key_; }
  // A key is on file (sealed, or at least its public half) — something that
  // 'wallet new' / 'wallet import' would replace.
  bool key_on_file() const { return !sealed_key_.empty() || !address_.empty(); }
  bool sealed_on_file() const { return !sealed_key_.empty(); }
  // Why the given passphrase did not open the sealed key, empty when it did.
  const std::string& unlock_error() const { return unlock_error_; }
  // How the sealed key on file is protected, e.g. "pbkdf2-sha256 200000 rounds".
  std::string kdf_description() const;

  chain::Network network() const { return network_; }
  void set_network(chain::Network n);

  const std::string& address() const { return address_; }         // p2wpkh, the one we spend from
  const std::string& legacy_address() const { return legacy_address_; }
  const std::string& pubkey_hex() const { return pubkey_hex_; }
  std::string export_wif() const;
  const std::string& eth_account() const { return eth_account_; }

  // An address whose key we do not hold. Recorded and reported as such; nothing
  // here can spend from it. Defaults to the compiled-in payout accounts.
  void watch(const std::string& address);
  const std::string& watched() const { return watched_; }
  void set_eth_payout(const std::string& account);
  const std::string& eth_payout() const { return eth_payout_; }

  void set_node(const std::string& url);
  const std::string& node_url() const { return node_url_; }

  uint64_t balance_sats() const { return balance_sats_; }
  double balance_btc() const { return (double)balance_sats_ / 100000000.0; }
  uint64_t watched_balance_sats() const { return watched_balance_sats_; }

  // Asks the configured node which outputs this key can spend. Requires a node;
  // there is deliberately no explorer fallback.
  Result<void> sync_from_node();
  bool live_synced() const { return live_synced_; }
  const std::string& live_sync_source() const { return live_sync_source_; }

  Result<BitcoinTransaction> create_and_sign_transaction(const std::string& recipient, uint64_t amount_sats,
                                                         uint64_t sat_per_vbyte = 2);

  // Signing happens inside the wallet so the secret never leaves it.
  Result<ByteBuffer> sign_ethereum(const chain::EthTransaction& tx) const;

  // Records an output a node has told us about, so a transaction can be built
  // and signed here without this process running an index of its own.
  void add_utxo(const UTXO& u);
  const std::vector<UTXO>& utxos() const { return utxos_; }
  const std::vector<BitcoinTransaction>& tx_history() const { return history_; }
  // Records a signed transaction and reserves the outputs it spends, so a second
  // send cannot sign the same coins again. The reservation lasts until a node
  // stops reporting the output (it was spent for real) or 'release' drops it.
  Result<void> record(const BitcoinTransaction& tx);
  // Drops the reservation a never-broadcast transaction holds. Returns how many
  // outputs became spendable again.
  size_t release(const std::string& txid);
  uint64_t reserved_sats() const;

 private:
  Kernel& kernel_;
  std::string wallet_name_;
  chain::Network network_ = chain::Network::Signet;

  bool has_key_ = false;
  std::array<uint8_t, 32> secret_{};
  std::string pubkey_hex_;
  std::string address_;
  std::string legacy_address_;
  std::string eth_account_;
  std::string watched_ = chain::PAYOUT_BTC;
  std::string eth_payout_ = chain::PAYOUT_ETH;
  std::string node_url_;
  std::string passphrase_;
  std::array<uint8_t, 16> salt_{};
  // The sealed key exactly as it sits on file, with how it was sealed. Kept so a
  // save by a process that never opened it writes it back instead of dropping it.
  std::string sealed_key_;
  std::string kdf_;          // "" = the legacy stretch (version 2 files)
  std::string nonce_hex_;    // "" = legacy: the nonce was the salt's first 12 bytes
  std::string unlock_error_;
  bool needs_reseal_ = false;
  std::vector<std::pair<std::string, uint32_t>> pending_spends_;   // outpoints of the last signed tx

  uint64_t balance_sats_ = 0;
  uint64_t watched_balance_sats_ = 0;
  bool live_synced_ = false;
  std::string live_sync_source_;
  std::vector<UTXO> utxos_;
  std::vector<BitcoinTransaction> history_;
  std::string root_path_;

  Result<void> derive_from_secret();
  Result<void> open_sealed();
  std::string state_path() const;
};

}  // namespace aios::userland
