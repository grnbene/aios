#include "aios/userland/miner.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
// A bare-metal kernel has one thread of execution per core and no thread
// library to ask for another, so the parallel path is compiled out there and
// the nonce range is walked on this core instead.
#include "aios/hal/sync.hpp"
#include <vector>
#include "aios/hal/host.hpp"

namespace aios::userland {

namespace {

uint64_t parse_u64_str(const std::string& s) {
  uint64_t v = 0;
  for (char c : s) if (c >= '0' && c <= '9') v = v * 10 + (uint64_t)(c - '0');
  return v;
}

uint32_t leading_zero_bits(const std::array<uint8_t, 32>& be) {
  uint32_t n = 0;
  for (uint8_t b : be) {
    if (b == 0) { n += 8; continue; }
    for (int i = 7; i >= 0 && !(b & (1u << i)); --i) ++n;
    break;
  }
  return n;
}

// nBits -> big-endian 256-bit target.
std::array<uint8_t, 32> expand_bits(uint32_t bits) {
  std::array<uint8_t, 32> t{};
  uint32_t exponent = bits >> 24;
  uint32_t mantissa = bits & 0x007fffff;
  for (int i = 0; i < 3; ++i) {
    int pos = 32 - (int)exponent + i;
    if (pos >= 0 && pos < 32) t[pos] = (uint8_t)(mantissa >> (8 * (2 - i)));
  }
  return t;
}

}  // namespace

BitcoinMiner::BitcoinMiner(Kernel& kernel) : kernel_(kernel), root_path_(kernel.config().root) {}

Result<void> BitcoinMiner::init() { return load_state(); }

HardwareProfile BitcoinMiner::inspect_hardware() const {
  HardwareProfile hw;
  const auto& topo = kernel_.host().topology();
  hw.cpus = topo.logical_cpus > 0 ? topo.logical_cpus : 1;
  hw.memory_bytes = topo.total_memory;
#if defined(__aarch64__)
  hw.cpu_arch = topo.arch.empty() ? "aarch64" : topo.arch;
#else
  hw.cpu_arch = topo.arch.empty() ? "x86_64" : topo.arch;
#endif
  return hw;
}

HashTemplate BitcoinMiner::local_template() const {
  HashTemplate t;
  uint32_t version = 0x20000000;
  std::memcpy(&t.header[0], &version, 4);
  // prev block: all zero — there is no chain behind this header.
  // The "merkle root" is a label plus the hashes done so far in this root, so
  // every run hashes a header no earlier run hashed (otherwise two runs in the
  // same second walk the same nonces and find the same best hash).
  const std::string label = "aios local hashing exercise: not network work #" + std::to_string(state_.total_hashes);
  auto merkle = crypto::sha256(ByteSpan(reinterpret_cast<const uint8_t*>(label.data()), label.size()));
  std::memcpy(&t.header[36], merkle.data(), 32);
  uint32_t ts = (uint32_t)(kernel_.host().wall_ns() / 1000000000ull);
  std::memcpy(&t.header[68], &ts, 4);
  std::memcpy(&t.header[72], &t.bits, 4);
  auto target = expand_bits(t.bits);
  t.target_hex = crypto::bytes_to_hex(ByteSpan(target.data(), 32));
  return t;
}

Result<HashRun> BitcoinMiner::hash_run(uint32_t max_threads, uint64_t nonces) {
  HardwareProfile hw = inspect_hardware();
  uint32_t threads = max_threads > 0 ? max_threads : hw.cpus;
  if (threads == 0) threads = 1;
  if (nonces == 0) return Result<HashRun>(Status::Invalid, "nothing to hash: nonces=0");
  if (nonces > 0x100000000ull) nonces = 0x100000000ull;   // the nonce field is 32 bits

  HashTemplate tpl = local_template();
  const auto target = expand_bits(tpl.bits);
  uint64_t started = kernel_.host().monotonic_ns();

  std::atomic<uint64_t> total_checked{0};
  std::atomic<uint64_t> meeting{0};
  hal::Mutex best_mutex;
  std::array<uint8_t, 32> best{};
  best.fill(0xff);

  auto worker_fn = [&](uint32_t tid) {
    std::array<uint8_t, 80> h = tpl.header;
    std::array<uint8_t, 32> local_best{};
    local_best.fill(0xff);
    uint64_t checked = 0, met = 0;
    for (uint64_t n = tid; n < nonces; n += threads) {
      uint32_t nonce = (uint32_t)n;
      h[76] = (uint8_t)nonce; h[77] = (uint8_t)(nonce >> 8); h[78] = (uint8_t)(nonce >> 16); h[79] = (uint8_t)(nonce >> 24);
      auto d = crypto::sha256d(ByteSpan(h.data(), 80));
      std::array<uint8_t, 32> be{};
      for (int i = 0; i < 32; ++i) be[i] = d[31 - i];
      ++checked;
      if (std::memcmp(be.data(), target.data(), 32) <= 0) ++met;
      if (std::memcmp(be.data(), local_best.data(), 32) < 0) local_best = be;
    }
    total_checked.fetch_add(checked, std::memory_order_relaxed);
    meeting.fetch_add(met, std::memory_order_relaxed);
    hal::LockGuard lock(best_mutex);
    if (std::memcmp(local_best.data(), best.data(), 32) < 0) best = local_best;
  };

#if defined(AIOS_METAL)
  for (uint32_t i = 0; i < threads; ++i) worker_fn(i);   // no threads here: the lanes run in turn
#else
  if (threads <= 1) {
    worker_fn(0);
  } else {
    std::vector<hal::Thread> workers;
    workers.reserve(threads);
    for (uint32_t i = 0; i < threads; ++i) workers.emplace_back([&worker_fn, i]() { worker_fn(i); });
    for (auto& t : workers) if (t.joinable()) t.join();
  }
#endif

  uint64_t ended = kernel_.host().monotonic_ns();
  HashRun run;
  run.hashes = total_checked.load();
  run.ns = ended > started ? ended - started : 0;
  run.threads = threads;
  run.meeting_target = meeting.load();
  run.best_hash_hex = crypto::bytes_to_hex(ByteSpan(best.data(), 32));
  run.best_zero_bits = leading_zero_bits(best);

  state_.total_hashes += run.hashes;
  state_.last_run_hashes = run.hashes;
  state_.last_run_ns = run.ns;
  state_.last_run_ended_ns = kernel_.host().wall_ns();
  state_.last_run_threads = threads;
  state_.total_meeting_target += run.meeting_target;
  if (state_.best_hash_hex.empty() || run.best_hash_hex < state_.best_hash_hex) {
    state_.best_hash_hex = run.best_hash_hex;
    state_.best_zero_bits = run.best_zero_bits;
  }
  auto saved = save_state();
  if (!saved.ok()) return Result<HashRun>(saved.error());
  return run;
}

Result<void> BitcoinMiner::load_state() {
  auto content_res = kernel_.host().read_file(root_path_ + "/miner.state");
  if (!content_res.ok()) return Ok();
  std::string content((const char*)content_res->data(), content_res->size());
  size_t start = 0;
  while (start < content.size()) {
    size_t end = content.find('\n', start);
    if (end == std::string::npos) end = content.size();
    std::string line = content.substr(start, end - start);
    start = end + 1;
    auto pos = line.find('=');
    if (pos == std::string::npos) continue;
    std::string key = line.substr(0, pos), val = line.substr(pos + 1);
    if (key == "total_hashes") state_.total_hashes = parse_u64_str(val);
    else if (key == "last_run_hashes") state_.last_run_hashes = parse_u64_str(val);
    else if (key == "last_run_ns") state_.last_run_ns = parse_u64_str(val);
    else if (key == "last_run_ended_ns") state_.last_run_ended_ns = parse_u64_str(val);
    else if (key == "last_run_threads") state_.last_run_threads = (uint32_t)parse_u64_str(val);
    else if (key == "best_hash") state_.best_hash_hex = val;
    else if (key == "best_zero_bits") state_.best_zero_bits = (uint32_t)parse_u64_str(val);
    else if (key == "meeting_diff1") state_.total_meeting_target = parse_u64_str(val);
    else if (key == "blocks_mined") state_.legacy_invented_blocks = parse_u64_str(val);
  }
  return Ok();
}

Result<void> BitcoinMiner::save_state() {
  std::string payload = "kind=local-sha256d-exercise (credits nothing)\n";
  payload += "total_hashes=" + std::to_string(state_.total_hashes) + "\n";
  payload += "last_run_hashes=" + std::to_string(state_.last_run_hashes) + "\n";
  payload += "last_run_ns=" + std::to_string(state_.last_run_ns) + "\n";
  payload += "last_run_ended_ns=" + std::to_string(state_.last_run_ended_ns) + "\n";
  payload += "last_run_threads=" + std::to_string(state_.last_run_threads) + "\n";
  payload += "best_hash=" + state_.best_hash_hex + "\n";
  payload += "best_zero_bits=" + std::to_string(state_.best_zero_bits) + "\n";
  payload += "meeting_diff1=" + std::to_string(state_.total_meeting_target) + "\n";
  // One file, rewritten in place. (This used to also create a new kernel state
  // object on every save, which nothing ever read and nothing ever freed.)
  return kernel_.host().write_file(root_path_ + "/miner.state",
                                   ByteSpan((const uint8_t*)payload.data(), payload.size()));
}

}  // namespace aios::userland
