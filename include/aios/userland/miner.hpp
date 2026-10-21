#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <array>
#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/kernel/kernel.hpp"

namespace aios::userland {

// A local SHA-256d hashing exercise, and nothing more. It hashes an 80-byte
// Bitcoin-format header built from a LOCAL template (no previous block, no
// transactions, no network work), so no nonce it finds is a block any network
// would accept and nothing is ever credited to anyone. What it reports is what
// it measured: how many hashes, how fast, the best hash seen, and how many met
// the difficulty-1 target. For the network's real target and what this machine
// could do against it, see `chain mine`. Real pool mining is not implemented.

struct HardwareProfile {
  uint32_t cpus = 1;
  uint64_t memory_bytes = 0;
  std::string cpu_arch;
};

struct HashTemplate {
  std::array<uint8_t, 80> header{};     // nonce bytes (76..79) are overwritten per hash
  uint32_t bits = 0x1d00ffff;           // difficulty 1: the easiest target Bitcoin ever enforced
  std::string target_hex;               // big-endian target the bits expand to
};

struct HashRun {
  uint64_t hashes = 0;
  uint64_t ns = 0;
  uint32_t threads = 0;
  uint64_t meeting_target = 0;          // hashes <= the difficulty-1 target
  std::string best_hash_hex;            // display order (big-endian)
  uint32_t best_zero_bits = 0;
};

struct MinerState {
  uint64_t total_hashes = 0;
  uint64_t last_run_hashes = 0;
  uint64_t last_run_ns = 0;
  uint64_t last_run_ended_ns = 0;
  uint32_t last_run_threads = 0;
  std::string best_hash_hex;
  uint32_t best_zero_bits = 0;
  uint64_t total_meeting_target = 0;
  // Earlier versions of this file recorded "blocks mined" and "rewards" that a
  // fixed easy target made up. They are read only to say so, never shown as real.
  uint64_t legacy_invented_blocks = 0;
};

class BitcoinMiner {
 public:
  explicit BitcoinMiner(Kernel& kernel);

  Result<void> init();
  Result<void> load_state();
  Result<void> save_state();

  HardwareProfile inspect_hardware() const;
  HashTemplate local_template() const;

  // Hashes `nonces` nonces of the local template across `threads` lanes.
  Result<HashRun> hash_run(uint32_t threads = 0, uint64_t nonces = 10000000);

  const MinerState& state() const { return state_; }
  double measured_hashrate() const {
    if (state_.last_run_ns == 0 || state_.last_run_hashes == 0) return 0.0;
    return static_cast<double>(state_.last_run_hashes) * 1e9 / static_cast<double>(state_.last_run_ns);
  }

 private:
  Kernel& kernel_;
  MinerState state_;
  std::string root_path_;
};

using Miner = BitcoinMiner;

}  // namespace aios::userland
