#include "aios/userland/bitcoin_contract.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include "aios/channel/btc_p2p.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/sync.hpp"
#include "aios/userland/chain/btc_tx.hpp"

namespace aios::userland {

namespace {

// A minimal script-number push for a lock time (CScriptNum, little endian).
std::string push_number(uint32_t v) {
  if (v == 0) return "00";
  if (v <= 16) { char b[3]; std::snprintf(b, sizeof b, "%02x", 0x50 + v); return b; }
  ByteBuffer n;
  while (v) { n.push_back((uint8_t)(v & 0xff)); v >>= 8; }
  if (n.back() & 0x80) n.push_back(0);
  char len[3]; std::snprintf(len, sizeof len, "%02x", (unsigned)n.size());
  return std::string(len) + crypto::bytes_to_hex(ByteSpan(n.data(), n.size()));
}

}  // namespace

BitcoinContractSolver::BitcoinContractSolver(Kernel& kernel, BitcoinWallet& wallet)
    : kernel_(kernel), wallet_(wallet) {}

std::string BitcoinContractSolver::preimage_for(uint32_t nonce) {
  return "preimage_nonce_" + std::to_string(nonce) + "_htlc";
}

std::string BitcoinContractSolver::htlc_script(const std::string& hash_hex, uint32_t lock_time) const {
  const std::string& pk = wallet_.pubkey_hex();
  if (pk.size() != 66 || hash_hex.size() != 64) return {};
  // 63 IF | a8 SHA256 | 20 <h> | 88 EQUALVERIFY | 21 <pk> | 67 ELSE |
  // <lock> | b1 CLTV | 75 DROP | 21 <pk> | 68 ENDIF | ac CHECKSIG
  return "63a820" + hash_hex + "8821" + pk + "67" + push_number(lock_time) + "b17521" + pk + "68ac";
}

ContractSpec BitcoinContractSolver::create_solvable_puzzle(uint32_t secret_nonce, uint32_t search_space,
                                                            uint32_t lock_time) {
  ContractSpec spec;
  spec.contract_id = "htlc_puzzle_" + std::to_string(kernel_.host().wall_ns() / 1000000ull);
  spec.type = ContractType::HTLC;
  spec.search_space = search_space;
  spec.lock_time = lock_time;
  std::string pre = preimage_for(secret_nonce);
  auto h = crypto::sha256(ByteSpan((const uint8_t*)pre.data(), pre.size()));
  spec.target_hash_hex = crypto::bytes_to_hex(ByteSpan(h.data(), 32));
  spec.script_hex = htlc_script(spec.target_hash_hex, lock_time);
  return spec;
}

bool BitcoinContractSolver::chain_tip(std::string& source_out, uint64_t& height_out) const {
  const auto& p = chain::network_params(wallet_.network());
  for (const auto& seed : channel::dns_seeds(p.magic)) {
    channel::BitcoinPeer peer;
    if (!peer.connect(seed, p.port, p.magic, 6000).ok()) continue;
    auto info = peer.handshake();
    if (!info.ok()) continue;
    source_out = "Bitcoin " + std::string(p.name) + " peer " + info->address + " " + info->subversion;
    height_out = (uint64_t)(info->start_height < 0 ? 0 : info->start_height);
    peer.close();
    return true;
  }
  return false;
}

Result<ContractSolution> BitcoinContractSolver::solve_contract(const ContractSpec& spec, bool force_gpu,
                                                               uint32_t max_threads) {
  // This build has no GPU SHA-256 kernel; a GPU request runs on the CPU and the
  // result says so.
  (void)force_gpu;
  auto target = crypto::hex_to_bytes(spec.target_hash_hex);
  if (target.size() != 32) return Result<ContractSolution>(Status::Invalid, "contract has no 32-byte target hash");
  if (spec.search_space == 0) return Result<ContractSolution>(Status::Invalid, "contract has no search space");

  uint32_t threads = max_threads;
  if (threads == 0) threads = kernel_.host().topology().logical_cpus;
  if (threads == 0) threads = 1;
  if (threads > spec.search_space) threads = spec.search_space;

  ContractSolution sol;
  sol.contract_id = spec.contract_id;
  sol.threads = threads;
  sol.accelerator_used = std::string("CPU scalar SHA-256 on ") + std::to_string(threads) + " thread(s)" +
                         (force_gpu ? " (GPU asked for; this build has no GPU SHA-256 kernel)" : "");

  std::atomic<bool> found{false};
  std::atomic<uint32_t> winner{0};
  std::atomic<uint64_t> evaluated{0};
  const uint64_t started = kernel_.host().monotonic_ns();
  auto worker = [&](uint32_t tid) {
    uint64_t n_done = 0;
    for (uint64_t n = tid; n < spec.search_space; n += threads) {
      if (found.load(std::memory_order_relaxed)) break;
      std::string pre = preimage_for((uint32_t)n);
      auto h = crypto::sha256(ByteSpan((const uint8_t*)pre.data(), pre.size()));
      ++n_done;
      if (std::memcmp(h.data(), target.data(), 32) == 0) {
        bool expected = false;
        if (found.compare_exchange_strong(expected, true)) winner.store((uint32_t)n);
        break;
      }
    }
    evaluated.fetch_add(n_done, std::memory_order_relaxed);
  };
#if defined(AIOS_METAL)
  for (uint32_t i = 0; i < threads; ++i) worker(i);
#else
  if (threads <= 1) {
    worker(0);
  } else {
    std::vector<hal::Thread> pool;
    pool.reserve(threads);
    for (uint32_t i = 0; i < threads; ++i) pool.emplace_back([&worker, i]() { worker(i); });
    for (auto& t : pool) if (t.joinable()) t.join();
  }
#endif
  const uint64_t ended = kernel_.host().monotonic_ns();
  sol.solve_ns = ended > started ? ended - started : 0;
  sol.hashes_evaluated = evaluated.load();
  if (found.load()) {
    sol.solved = true;
    sol.winning_nonce = winner.load();
    std::string pre = preimage_for(sol.winning_nonce);
    sol.preimage_hex = crypto::bytes_to_hex(ByteSpan((const uint8_t*)pre.data(), pre.size()));
    sol.solution_hash_hex = spec.target_hash_hex;
  }
  return sol;
}

Result<bool> BitcoinContractSolver::evaluate_script(const std::string& script_hex,
                                                   const std::vector<std::string>& stack_items) {
  if (script_hex.empty()) return Result<bool>(Status::Invalid, "no script to evaluate");
  ByteBuffer script = crypto::hex_to_bytes(script_hex);
  std::vector<ByteBuffer> stack;
  for (const auto& item : stack_items) stack.push_back(crypto::hex_to_bytes(item));
  chain::ScriptContext ctx;
  return chain::evaluate_script_stack(stack, ByteSpan(script.data(), script.size()), ctx);
}

}  // namespace aios::userland
