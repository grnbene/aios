#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/userland/bitcoin_wallet.hpp"

namespace aios::userland {

// Hash-locked contracts built and checked on this machine. Nothing here is on
// any chain: a contract is a script and a hash, nothing funds it, and solving it
// pays nobody. What it can show for real: a well-formed HTLC script, a preimage
// search that runs on the lanes it says, and the preimage checked by the script
// interpreter.
enum class ContractType { HashLock, HTLC };

struct ContractSpec {
  std::string contract_id;
  ContractType type = ContractType::HTLC;
  std::string target_hash_hex;   // sha256 of the preimage
  std::string script_hex;        // empty when there is no public key to lock to
  uint32_t lock_time = 0;
  uint32_t search_space = 0;     // nonces the preimage is known to lie within
};

struct ContractSolution {
  std::string contract_id;
  bool solved = false;
  std::string preimage_hex;
  uint32_t winning_nonce = 0;
  std::string solution_hash_hex;
  uint64_t solve_ns = 0;
  std::string accelerator_used;
  uint32_t threads = 0;
  uint64_t hashes_evaluated = 0;
};

class BitcoinContractSolver {
 public:
  BitcoinContractSolver(Kernel& kernel, BitcoinWallet& wallet);

  // The preimage the solvable puzzle uses for nonce n.
  static std::string preimage_for(uint32_t nonce);

  // OP_IF OP_SHA256 <h> OP_EQUALVERIFY <pk> OP_ELSE <lock> OP_CHECKLOCKTIMEVERIFY
  // OP_DROP <pk> OP_ENDIF OP_CHECKSIG, locked to this wallet's public key. Empty
  // when the wallet has no public key on file.
  std::string htlc_script(const std::string& hash_hex, uint32_t lock_time) const;

  // A puzzle whose preimage is preimage_for(secret) with secret < search_space.
  ContractSpec create_solvable_puzzle(uint32_t secret_nonce, uint32_t search_space, uint32_t lock_time = 0);

  // The tip height a peer of the wallet's network claims in its handshake.
  bool chain_tip(std::string& source_out, uint64_t& height_out) const;

  // Searches preimage_for(0 .. search_space) on `threads` lanes (0 = all).
  Result<ContractSolution> solve_contract(const ContractSpec& spec, bool force_gpu = false, uint32_t threads = 0);

  // Runs `script_hex` in the script interpreter with `stack_items` (hex) pushed.
  Result<bool> evaluate_script(const std::string& script_hex, const std::vector<std::string>& stack_items);

 private:
  Kernel& kernel_;
  BitcoinWallet& wallet_;
};

}  // namespace aios::userland
