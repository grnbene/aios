#include "aios/userland/bitcoin_wallet.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include "aios/channel/chain_rpc.hpp"
#include "aios/crypto/base58check.hpp"
#include "aios/crypto/hmac_sha256.hpp"
#include "aios/crypto/secp256k1.hpp"
#include "aios/hal/host.hpp"
#include "aios/node/secure.hpp"
#include "aios/userland/chain/eth_tx.hpp"

namespace aios::userland {

using namespace aios::crypto;

namespace {

// The passphrase KDF for keys sealed from now on: PBKDF2-HMAC-SHA256 (RFC 8018).
constexpr uint32_t kPbkdf2Rounds = 210000;
const char* const kKdfName = "pbkdf2-sha256";

crypto::Key pbkdf2_key(const std::string& passphrase, ByteSpan salt, uint32_t rounds) {
  crypto::Key key{};
  pbkdf2_hmac_sha256(ByteSpan(reinterpret_cast<const uint8_t*>(passphrase.data()), passphrase.size()), salt,
                     rounds, key.data(), key.size());
  return key;
}

// The construction version-2 wallets were sealed with (the node-link token
// stretch, re-run 64 times). Kept only to open those files; the next save
// re-seals them with PBKDF2.
crypto::Key legacy_stretch(const std::string& passphrase, ByteSpan salt) {
  crypto::Key key = derive_key(passphrase, salt);
  for (int i = 0; i < 64; ++i) {
    std::string round(reinterpret_cast<const char*>(key.data()), key.size());
    key = derive_key(round, salt);
  }
  return key;
}

std::string field_of(const std::string& content, const std::string& key) {
  size_t start = 0;
  while (start < content.size()) {
    size_t end = content.find('\n', start);
    if (end == std::string::npos) end = content.size();
    std::string line = content.substr(start, end - start);
    start = end + 1;
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    if (line.substr(0, eq) == key) return line.substr(eq + 1);
  }
  return {};
}

}  // namespace

BitcoinWallet::BitcoinWallet(Kernel& kernel) : kernel_(kernel), root_path_(kernel.config().root) {}

std::string BitcoinWallet::state_path() const {
  return root_path_ + "/wallet_" + wallet_name_ + ".state";
}

void BitcoinWallet::set_network(chain::Network n) {
  network_ = n;
  if (has_key_) {
    (void)derive_from_secret();
    return;
  }
  // No secret in hand, but the public key on file is enough to say what this
  // key's addresses are on the other network.
  auto pub = hex_to_bytes(pubkey_hex_);
  if (pub.size() == 33) {
    auto p2wpkh = chain::address_p2wpkh(ByteSpan(pub.data(), pub.size()), network_);
    if (p2wpkh.ok()) address_ = *p2wpkh;
    auto p2pkh = chain::address_p2pkh(ByteSpan(pub.data(), pub.size()), network_);
    if (p2pkh.ok()) legacy_address_ = *p2pkh;
  }
}

void BitcoinWallet::unlock(const std::string& passphrase) {
  passphrase_ = passphrase;
  unlock_error_.clear();
  if (!passphrase_.empty() && !sealed_key_.empty() && !has_key_) (void)open_sealed();
}

std::string BitcoinWallet::kdf_description() const {
  if (sealed_key_.empty() && !(has_key_ && unlocked())) return "no key sealed on file";
  if (kdf_.empty()) return "legacy stretch (version 2) — re-sealed with PBKDF2 on the next save with the passphrase";
  return kdf_;
}

Result<void> BitcoinWallet::open_sealed() {
  unlock_error_.clear();
  auto blob = hex_to_bytes(sealed_key_);
  crypto::Key key{};
  crypto::Nonce nonce{};
  if (kdf_.empty()) {
    key = legacy_stretch(passphrase_, ByteSpan(salt_.data(), salt_.size()));
    std::memcpy(nonce.data(), salt_.data(), 12);
  } else {
    // "pbkdf2-sha256:<rounds>"
    uint32_t rounds = 0;
    auto colon = kdf_.find(':');
    if (kdf_.rfind(kKdfName, 0) != 0 || colon == std::string::npos) {
      unlock_error_ = "the key on file is sealed with an unknown kdf '" + kdf_ + "'";
      return Result<void>(Status::Unsupported, unlock_error_);
    }
    for (char ch : kdf_.substr(colon + 1)) if (ch >= '0' && ch <= '9') rounds = rounds * 10 + (uint32_t)(ch - '0');
    auto nb = hex_to_bytes(nonce_hex_);
    if (rounds == 0 || nb.size() != 12) {
      unlock_error_ = "the sealed key's kdf rounds or nonce on file are malformed";
      return Result<void>(Status::Corrupt, unlock_error_);
    }
    key = pbkdf2_key(passphrase_, ByteSpan(salt_.data(), salt_.size()), rounds);
    std::memcpy(nonce.data(), nb.data(), 12);
  }
  auto opened = aead_decrypt(key, nonce, ByteSpan(), ByteSpan(blob.data(), blob.size()));
  if (!opened.ok() || opened->size() != 32) {
    unlock_error_ = "wrong passphrase: it does not open the key sealed on file";
    return Result<void>(Status::Denied, unlock_error_);
  }
  std::memcpy(secret_.data(), opened->data(), 32);
  auto derived = derive_from_secret();
  if (!derived.ok()) return derived;
  if (kdf_.empty()) needs_reseal_ = true;
  return Ok();
}

void BitcoinWallet::watch(const std::string& address) {
  watched_ = address;
  watched_balance_sats_ = 0;
}

void BitcoinWallet::set_eth_payout(const std::string& account) { eth_payout_ = account; }

void BitcoinWallet::set_node(const std::string& url) { node_url_ = url; }

Result<void> BitcoinWallet::init(const std::string& wallet_name, chain::Network network) {
  wallet_name_ = wallet_name;
  network_ = network;
  (void)load_state();
  // A version-2 file opened with its passphrase is re-sealed under PBKDF2 now.
  if (needs_reseal_ && has_key_ && unlocked()) {
    needs_reseal_ = false;
    return save_state();
  }
  return Ok();
}

Result<void> BitcoinWallet::derive_from_secret() {
  auto pubkey = ec_pubkey_create(secret_.data(), true);
  if (!pubkey.ok()) return Result<void>(pubkey.error());
  pubkey_hex_ = bytes_to_hex(ByteSpan(pubkey->data(), pubkey->size()));

  auto p2wpkh = chain::address_p2wpkh(ByteSpan(pubkey->data(), pubkey->size()), network_);
  if (!p2wpkh.ok()) return Result<void>(p2wpkh.error());
  address_ = *p2wpkh;

  auto p2pkh = chain::address_p2pkh(ByteSpan(pubkey->data(), pubkey->size()), network_);
  if (p2pkh.ok()) legacy_address_ = *p2pkh;

  auto eth = chain::eth_address_of_key(secret_.data());
  if (eth.ok()) eth_account_ = chain::eth_checksum_address(*eth);

  has_key_ = true;
  return Ok();
}

Result<void> BitcoinWallet::generate_new_keys() {
  for (int attempt = 0; attempt < 16; ++attempt) {
    kernel_.host().random(MutableByteSpan(secret_.data(), 32));
    if (!ec_seckey_valid(secret_.data())) continue;
    kernel_.host().random(MutableByteSpan(salt_.data(), salt_.size()));
    auto derived = derive_from_secret();
    if (!derived.ok()) return derived;
    balance_sats_ = 0;
    utxos_.clear();
    history_.clear();
    sealed_key_.clear();
    unlock_error_.clear();
    return save_state();
  }
  return Result<void>(Status::IoError, "the host's entropy did not yield a usable key");
}

Result<void> BitcoinWallet::import_wif(const std::string& wif) {
  uint8_t version = 0;
  auto payload = base58check_decode(wif, version);
  if (!payload.ok()) return Result<void>(payload.error());
  if (payload->size() != 32 && payload->size() != 33) {
    return Result<void>(Status::Invalid, "a wif key carries 32 bytes, optionally with a compression flag");
  }
  if (version != chain::network_params(network_).wif_version) {
    return Result<void>(Status::Invalid, "this wif belongs to a different network");
  }
  std::memcpy(secret_.data(), payload->data(), 32);
  if (!ec_seckey_valid(secret_.data())) return Result<void>(Status::Invalid, "wif key is out of range");

  kernel_.host().random(MutableByteSpan(salt_.data(), salt_.size()));
  auto derived = derive_from_secret();
  if (!derived.ok()) return derived;
  balance_sats_ = 0;
  utxos_.clear();
  history_.clear();
  sealed_key_.clear();
  unlock_error_.clear();
  return save_state();
}

std::string BitcoinWallet::export_wif() const {
  if (!has_key_) return {};
  ByteBuffer payload(secret_.begin(), secret_.end());
  payload.push_back(0x01);
  return base58check_encode(chain::network_params(network_).wif_version,
                            ByteSpan(payload.data(), payload.size()));
}

Result<void> BitcoinWallet::sync_from_node() {
  live_synced_ = false;
  live_sync_source_.clear();
  if (node_url_.empty()) {
    return Result<void>(Status::Invalid,
                        "no node configured; set one with 'wallet node <url>' — there is no explorer fallback");
  }
  if (!has_key_) return Result<void>(Status::Invalid, "wallet holds no key");

  channel::ChainRpc rpc;
  // The host boundary goes along so an https:// node is reached over TLS.
  auto opened = rpc.open(node_url_, 60000, &kernel_.host(), root_path_);
  if (!opened.ok()) return opened;

  channel::RpcValue descriptors = channel::RpcValue::list();
  descriptors.push(channel::RpcValue::text("addr(" + address_ + ")"));
  if (!legacy_address_.empty()) descriptors.push(channel::RpcValue::text("addr(" + legacy_address_ + ")"));

  auto scan = rpc.call("scantxoutset", {channel::RpcValue::text("start"), descriptors});
  if (!scan.ok()) return Result<void>(scan.error());

  // Reservations survive a sync while the node still reports the output as
  // unspent (the signing transaction has not confirmed, or was never sent). An
  // output the node no longer reports is gone, and its reservation with it.
  std::vector<UTXO> previous = utxos_;
  utxos_.clear();
  balance_sats_ = 0;
  const auto& found = (*scan)["unspents"];
  for (size_t i = 0; i < found.size(); ++i) {
    const auto& entry = found[i];
    UTXO u;
    u.txid = entry["txid"].as_text();
    u.vout = (uint32_t)entry["vout"].as_u64();
    u.script_pubkey = entry["scriptPubKey"].as_text();
    u.amount_sats = (uint64_t)(entry["amount"].as_double() * 100000000.0 + 0.5);
    for (const auto& p : previous)
      if (p.txid == u.txid && p.vout == u.vout) u.reserved_by = p.reserved_by;
    if (u.reserved_by.empty()) balance_sats_ += u.amount_sats;
    utxos_.push_back(u);
  }

  live_synced_ = true;
  live_sync_source_ = "node at " + rpc.describe();

  if (!watched_.empty()) {
    channel::RpcValue watch_list = channel::RpcValue::list();
    watch_list.push(channel::RpcValue::text("addr(" + watched_ + ")"));
    auto watch_scan = rpc.call("scantxoutset", {channel::RpcValue::text("start"), watch_list});
    if (watch_scan.ok()) {
      watched_balance_sats_ =
          (uint64_t)((*watch_scan)["total_amount"].as_double() * 100000000.0 + 0.5);
    }
  }

  (void)save_state();
  return Ok();
}

Result<BitcoinTransaction> BitcoinWallet::create_and_sign_transaction(const std::string& recipient,
                                                                     uint64_t amount_sats,
                                                                     uint64_t sat_per_vbyte) {
  if (!has_key_) return Result<BitcoinTransaction>(Status::Invalid, "wallet holds no key");
  if (utxos_.empty()) {
    return Result<BitcoinTransaction>(Status::Exhausted,
                                      "no spendable outputs; sync from a node before spending");
  }

  std::vector<chain::SpendableOutput> available;
  for (const auto& u : utxos_) {
    if (u.spent || !u.reserved_by.empty()) continue;
    auto txid = chain::hash_from_display(u.txid);
    if (!txid.ok()) continue;
    chain::SpendableOutput s;
    s.prevout.txid = *txid;
    s.prevout.index = u.vout;
    s.amount = u.amount_sats;
    s.script_pubkey = hex_to_bytes(u.script_pubkey);
    s.witness = !s.script_pubkey.empty() && s.script_pubkey[0] == 0x00;
    available.push_back(s);
  }

  if (available.empty()) {
    return Result<BitcoinTransaction>(Status::Exhausted,
                                      "every output is reserved by a transaction signed earlier; 'wallet history' "
                                      "lists them, 'wallet release <txid>' frees one that was never broadcast");
  }
  auto plan = chain::build_payment(available, recipient, amount_sats, address_, sat_per_vbyte, network_);
  if (!plan.ok()) return Result<BitcoinTransaction>(plan.error());

  auto signed_ok = chain::sign_payment(*plan, secret_.data());
  if (!signed_ok.ok()) return Result<BitcoinTransaction>(signed_ok.error());

  auto raw = chain::serialize_tx(plan->tx, true);

  BitcoinTransaction tx;
  tx.txid = chain::hash_to_display(chain::txid_of(plan->tx));
  tx.raw_hex = bytes_to_hex(ByteSpan(raw.data(), raw.size()));
  tx.fee_sats = plan->fee;
  tx.timestamp = kernel_.now();
  tx.recipient = recipient;
  tx.amount_sats = amount_sats;
  tx.vsize = chain::vsize_of(plan->tx);
  pending_spends_.clear();
  for (const auto& in : plan->tx.inputs)
    pending_spends_.emplace_back(chain::hash_to_display(in.prevout.txid), in.prevout.index);
  return tx;
}

Result<ByteBuffer> BitcoinWallet::sign_ethereum(const chain::EthTransaction& tx) const {
  if (!has_key_) return Result<ByteBuffer>(Status::Invalid, "wallet holds no key");
  return chain::eth_sign_1559(tx, secret_.data());
}

void BitcoinWallet::add_utxo(const UTXO& u) {
  for (const auto& have : utxos_)
    if (have.txid == u.txid && have.vout == u.vout) return;   // already known
  utxos_.push_back(u);
  if (u.reserved_by.empty()) balance_sats_ += u.amount_sats;
  (void)save_state();
}

Result<void> BitcoinWallet::record(const BitcoinTransaction& tx) {
  for (auto& u : utxos_) {
    for (const auto& sp : pending_spends_) {
      if (u.txid == sp.first && u.vout == sp.second && u.reserved_by.empty()) {
        u.reserved_by = tx.txid;
        balance_sats_ = balance_sats_ >= u.amount_sats ? balance_sats_ - u.amount_sats : 0;
      }
    }
  }
  pending_spends_.clear();
  history_.push_back(tx);
  return save_state();
}

size_t BitcoinWallet::release(const std::string& txid) {
  size_t freed = 0;
  for (auto& u : utxos_) {
    if (!u.reserved_by.empty() && u.reserved_by == txid) {
      u.reserved_by.clear();
      balance_sats_ += u.amount_sats;
      ++freed;
    }
  }
  if (freed) (void)save_state();
  return freed;
}

uint64_t BitcoinWallet::reserved_sats() const {
  uint64_t total = 0;
  for (const auto& u : utxos_)
    if (!u.reserved_by.empty()) total += u.amount_sats;
  return total;
}

Result<void> BitcoinWallet::load_state() {
  auto content_res = kernel_.host().read_file(state_path());
  if (!content_res.ok()) return Ok();
  std::string content((const char*)content_res->data(), content_res->size());

  std::string net = field_of(content, "network");
  if (!net.empty()) {
    auto parsed = chain::network_by_name(net);
    if (parsed.ok()) network_ = *parsed;
  }
  std::string stored_watch = field_of(content, "watch");
  if (!stored_watch.empty()) watched_ = stored_watch;
  std::string stored_payout = field_of(content, "eth_payout");
  if (!stored_payout.empty()) eth_payout_ = stored_payout;
  node_url_ = field_of(content, "node");

  std::string salt_hex = field_of(content, "salt");
  if (salt_hex.size() == 32) {
    auto bytes = hex_to_bytes(salt_hex);
    if (bytes.size() == 16) std::memcpy(salt_.data(), bytes.data(), 16);
  }

  sealed_key_ = field_of(content, "key");
  kdf_ = field_of(content, "kdf");
  nonce_hex_ = field_of(content, "nonce");
  unlock_error_.clear();
  has_key_ = false;
  if (!sealed_key_.empty() && !passphrase_.empty()) (void)open_sealed();

  if (!has_key_) {
    address_ = field_of(content, "address");
    legacy_address_ = field_of(content, "legacy");
    pubkey_hex_ = field_of(content, "pubkey");
    eth_account_ = field_of(content, "eth");
  }

  balance_sats_ = 0;
  utxos_.clear();
  size_t at = 0;
  while (at < content.size()) {
    size_t end = content.find('\n', at);
    if (end == std::string::npos) end = content.size();
    std::string line = content.substr(at, end - at);
    at = end + 1;
    if (line.rfind("utxo=", 0) != 0) continue;

    std::string body = line.substr(5);
    std::vector<std::string> parts;
    size_t from = 0;
    while (from <= body.size()) {
      size_t colon = body.find(':', from);
      if (colon == std::string::npos) { parts.push_back(body.substr(from)); break; }
      parts.push_back(body.substr(from, colon - from));
      from = colon + 1;
    }
    if (parts.size() != 4 && parts.size() != 5) continue;

    UTXO u;
    u.txid = parts[0];
    u.vout = 0;
    for (char ch : parts[1]) { if (ch >= '0' && ch <= '9') u.vout = u.vout * 10 + (uint32_t)(ch - '0'); }
    u.amount_sats = 0;
    for (char ch : parts[2]) { if (ch >= '0' && ch <= '9') u.amount_sats = u.amount_sats * 10 + (uint64_t)(ch - '0'); }
    u.script_pubkey = parts[3];
    if (parts.size() == 5) u.reserved_by = parts[4];
    if (u.reserved_by.empty()) balance_sats_ += u.amount_sats;
    utxos_.push_back(u);
  }

  // tx=<txid>:<amount>:<fee>:<vsize>:<timestamp>:<recipient>:<raw hex>
  history_.clear();
  at = 0;
  while (at < content.size()) {
    size_t end = content.find('\n', at);
    if (end == std::string::npos) end = content.size();
    std::string line = content.substr(at, end - at);
    at = end + 1;
    if (line.rfind("tx=", 0) != 0) continue;
    std::vector<std::string> parts;
    std::string body = line.substr(3);
    size_t from = 0;
    while (true) {
      size_t colon = body.find(':', from);
      if (colon == std::string::npos || parts.size() == 6) { parts.push_back(body.substr(from)); break; }
      parts.push_back(body.substr(from, colon - from));
      from = colon + 1;
    }
    if (parts.size() != 7) continue;
    BitcoinTransaction tx;
    tx.txid = parts[0];
    tx.amount_sats = std::strtoull(parts[1].c_str(), nullptr, 10);
    tx.fee_sats = std::strtoull(parts[2].c_str(), nullptr, 10);
    tx.vsize = std::strtoull(parts[3].c_str(), nullptr, 10);
    tx.timestamp = std::strtoull(parts[4].c_str(), nullptr, 10);
    tx.recipient = parts[5];
    tx.raw_hex = parts[6];
    history_.push_back(tx);
  }
  return Ok();
}

Result<void> BitcoinWallet::save_state() {
  std::string payload = "version=3\n";
  payload += "network=" + std::string(chain::network_params(network_).name) + "\n";
  payload += "address=" + address_ + "\n";
  payload += "legacy=" + legacy_address_ + "\n";
  payload += "pubkey=" + pubkey_hex_ + "\n";
  payload += "eth=" + eth_account_ + "\n";
  payload += "watch=" + watched_ + "\n";
  payload += "eth_payout=" + eth_payout_ + "\n";
  payload += "node=" + node_url_ + "\n";
  payload += "balance_sats=" + std::to_string(balance_sats_) + "\n";
  for (const auto& u : utxos_) {
    if (u.spent) continue;
    payload += "utxo=" + u.txid + ":" + std::to_string(u.vout) + ":" + std::to_string(u.amount_sats) + ":" +
               u.script_pubkey + (u.reserved_by.empty() ? std::string() : ":" + u.reserved_by) + "\n";
  }
  for (const auto& tx : history_) {
    payload += "tx=" + tx.txid + ":" + std::to_string(tx.amount_sats) + ":" + std::to_string(tx.fee_sats) + ":" +
               std::to_string(tx.vsize) + ":" + std::to_string(tx.timestamp) + ":" + tx.recipient + ":" +
               tx.raw_hex + "\n";
  }

  if (has_key_ && !passphrase_.empty() && unlock_error_.empty()) {
    // Fresh salt and nonce on every seal: the nonce is never reused under a key.
    kernel_.host().random(MutableByteSpan(salt_.data(), salt_.size()));
    crypto::Nonce nonce{};
    kernel_.host().random(MutableByteSpan(nonce.data(), nonce.size()));
    crypto::Key key = pbkdf2_key(passphrase_, ByteSpan(salt_.data(), salt_.size()), kPbkdf2Rounds);
    auto sealed = aead_encrypt(key, nonce, ByteSpan(), ByteSpan(secret_.data(), 32));
    sealed_key_ = bytes_to_hex(ByteSpan(sealed.data(), sealed.size()));
    kdf_ = std::string(kKdfName) + ":" + std::to_string(kPbkdf2Rounds);
    nonce_hex_ = bytes_to_hex(ByteSpan(nonce.data(), nonce.size()));
    payload += "salt=" + bytes_to_hex(ByteSpan(salt_.data(), salt_.size())) + "\n";
    payload += "kdf=" + kdf_ + "\n";
    payload += "nonce=" + nonce_hex_ + "\n";
    payload += "key=" + sealed_key_ + "\n";
  } else if (!sealed_key_.empty()) {
    // This process never opened the sealed key (no passphrase, or the wrong
    // one). It is written back exactly as it was read, never dropped.
    payload += "salt=" + bytes_to_hex(ByteSpan(salt_.data(), salt_.size())) + "\n";
    if (!kdf_.empty()) payload += "kdf=" + kdf_ + "\n";
    if (!nonce_hex_.empty()) payload += "nonce=" + nonce_hex_ + "\n";
    payload += "key=" + sealed_key_ + "\n";
  } else if (has_key_) {
    payload += "key=\n";
    payload += "note=no passphrase was set, so the secret was kept in memory and not written\n";
  }

  const std::string path = state_path();
  auto wrote = kernel_.host().write_file(path, ByteSpan((const uint8_t*)payload.data(), payload.size()));
  if (!wrote.ok()) return wrote;
  // Owner-only: the file carries the sealed key and the wallet's addresses.
  auto narrowed = kernel_.host().set_access(path, hal::Host::FileAccess::Private);
  if (!narrowed.ok()) return narrowed;
  return Ok();
}

}  // namespace aios::userland
