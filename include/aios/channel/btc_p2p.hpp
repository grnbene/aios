#pragma once
#include <array>
#include <memory>
#include <string>
#include <vector>
#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"
#include "aios/hal/net.hpp"

namespace aios::channel {

struct BlockHeader {
  int32_t version = 0;
  std::array<uint8_t, 32> prev_block{};
  std::array<uint8_t, 32> merkle_root{};
  uint32_t time = 0;
  uint32_t bits = 0;
  uint32_t nonce = 0;

  ByteBuffer serialize() const;
  std::array<uint8_t, 32> hash() const;
  // Checks the header's own digest against the target its nBits declares. It
  // does not check that the target is the one the chain's retarget rule
  // demands, which needs the previous 2016 headers.
  bool work_is_valid() const;
};

struct PeerInfo {
  std::string address;
  int32_t version = 0;
  uint64_t services = 0;
  std::string subversion;
  int32_t start_height = 0;
};

struct P2pMessage {
  std::string command;
  ByteBuffer payload;
};

enum : uint32_t {
  INV_TX = 1,
  INV_BLOCK = 2,
  INV_WITNESS_TX = 0x40000001u,
};

// One connection to one Bitcoin peer, speaking the wire protocol directly over
// hal::net. No explorer, no gateway: the tip this reports is one the peer
// signed off on and this process verified.
class BitcoinPeer {
 public:
  BitcoinPeer() = default;
  ~BitcoinPeer();

  Result<void> connect(const std::string& host, uint16_t port, uint32_t magic, int timeout_ms = 15000);
  Result<PeerInfo> handshake(int32_t start_height = 0, int timeout_ms = 20000);

  Result<std::vector<BlockHeader>> get_headers(const std::vector<std::array<uint8_t, 32>>& locator,
                                               int timeout_ms = 30000);

  // Announces the transaction and, if the peer asks for it, hands it over.
  // Returns true only when the peer actually requested the body, which is the
  // only evidence from this side that it wanted the transaction.
  Result<bool> offer_transaction(ByteSpan raw_tx, const std::array<uint8_t, 32>& txid,
                                 const std::array<uint8_t, 32>& wtxid, int timeout_ms = 30000);

  // Asks the peer whether it is holding the transaction. A peer that answers
  // with the body has it in its mempool.
  Result<bool> has_transaction(const std::array<uint8_t, 32>& txid, int timeout_ms = 20000);

  Result<void> send(const std::string& command, ByteSpan payload);
  Result<P2pMessage> receive(int timeout_ms);

  const PeerInfo& info() const { return info_; }
  bool connected() const { return conn_ != nullptr; }
  void close();

 private:
  std::unique_ptr<hal::Connection> conn_;
  uint32_t magic_ = 0;
  PeerInfo info_;
  uint64_t nonce_ = 0;

  Result<P2pMessage> await_command(const std::vector<std::string>& wanted, int timeout_ms);
};

// The work a target implies, relative to the easiest target the chain allows.
double difficulty_of(uint32_t bits);

struct HashrateSample {
  uint64_t hashes = 0;
  double hashes_per_second = 0.0;
  std::string best_hash;   // the closest this run came to the target
};

// Hashes a real header with real SHA-256d across the given threads for the
// given seconds, and reports what actually happened.
HashrateSample measure_hashrate(const BlockHeader& header, uint32_t threads, uint32_t seconds);

// The peers a cold node starts from. Bitcoin publishes these as DNS names whose
// A records are recently seen peers.
std::vector<std::string> dns_seeds(uint32_t magic);

}  // namespace aios::channel
