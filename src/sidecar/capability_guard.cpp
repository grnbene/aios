#include "aios/sidecar/capability_guard.hpp"
#include "aios/core/numparse.hpp"
#include "aios/crypto/hmac_sha256.hpp"
#include "aios/hal/sync.hpp"
#include "aios/hal/host.hpp"
#include "aios/crypto/sha256.hpp"
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace aios::sidecar {

namespace {
std::string hex_encode(const std::string& in) {
  static const char* d = "0123456789abcdef";
  std::string o;
  o.reserve(in.size() * 2);
  for (unsigned char c : in) { o += d[c >> 4]; o += d[c & 15]; }
  return o;
}
bool hex_decode(const std::string& in, std::string* out) {
  if (in.size() % 2) return false;
  auto nib = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
  out->clear();
  for (size_t i = 0; i < in.size(); i += 2) {
    int h = nib(in[i]), l = nib(in[i + 1]);
    if (h < 0 || l < 0) return false;
    out->push_back((char)(h * 16 + l));
  }
  return true;
}
uint64_t generation_sum(const std::unordered_map<std::string, uint32_t>& m) {
  uint64_t t = 0;
  for (const auto& kv : m) t += kv.second;
  return t;
}
}  // namespace

CapabilityGuard::CapabilityGuard(const std::string& master_secret, const std::string& state_dir)
    : secret_(master_secret) {
  if (state_dir.empty()) return;
  (void)aios::hal::process_host().make_dir(state_dir);
  generations_file_ = state_dir + "/generations.dat";
  load_generations();
}

// The store is `<hex name>\t<generation>\n` lines, then `mac\t<hex>\n`: the
// HMAC of those lines under the guard's own key. Anyone able to write the
// directory can neither lower a generation (un-revoking a token) nor plant one
// without the key, and a half-written file is told apart from a good one.
std::string CapabilityGuard::encode_generations() const {
  std::string body;
  for (const auto& [name, gen] : resource_generations_) body += hex_encode(name) + "\t" + std::to_string(gen) + "\n";
  aios::ByteSpan key{reinterpret_cast<const uint8_t*>(secret_.data()), secret_.size()};
  aios::ByteSpan msg{reinterpret_cast<const uint8_t*>(body.data()), body.size()};
  auto mac = aios::crypto::hmac_sha256(key, msg);
  return body + "mac\t" + hex_encode(std::string(reinterpret_cast<const char*>(mac.data()), mac.size())) + "\n";
}

bool CapabilityGuard::decode_generations(const std::string& content, std::unordered_map<std::string, uint32_t>* out) const {
  const size_t mac_at = content.rfind("mac\t");
  if (mac_at == std::string::npos || (mac_at != 0 && content[mac_at - 1] != '\n')) return false;
  const std::string body = content.substr(0, mac_at);
  std::string mac_hex = content.substr(mac_at + 4);
  while (!mac_hex.empty() && (mac_hex.back() == '\n' || mac_hex.back() == '\r')) mac_hex.pop_back();
  std::string have;
  if (!hex_decode(mac_hex, &have)) return false;
  aios::ByteSpan key{reinterpret_cast<const uint8_t*>(secret_.data()), secret_.size()};
  aios::ByteSpan msg{reinterpret_cast<const uint8_t*>(body.data()), body.size()};
  auto want = aios::crypto::hmac_sha256(key, msg);
  if (have.size() != want.size()) return false;
  unsigned char diff = 0;
  for (size_t k = 0; k < want.size(); ++k) diff |= static_cast<unsigned char>(have[k] ^ static_cast<char>(want[k]));
  if (diff != 0) return false;
  out->clear();
  size_t pos = 0;
  while (pos < body.size()) {
    size_t nl = body.find('\n', pos);
    if (nl == std::string::npos) return false;
    const std::string line = body.substr(pos, nl - pos);
    pos = nl + 1;
    const size_t tab = line.find('\t');
    std::string name;
    if (tab == std::string::npos || !hex_decode(line.substr(0, tab), &name)) return false;
    const char* end = nullptr;
    unsigned long gen = (unsigned long)aios::num::to_ull(line.c_str() + tab + 1, &end, 10);
    if (end == line.c_str() + tab + 1 || *end != '\0' || gen == 0) return false;
    (*out)[name] = (uint32_t)gen;
  }
  return true;
}

// Two files are written in turn (.new, then the store) because the host has no
// atomic rename: a crash in the middle of either leaves the other whole, and
// the one holding the higher generations is the newer.
void CapabilityGuard::load_generations() {
  bool any_file = false;
  std::unordered_map<std::string, uint32_t> best;
  bool have_best = false;
  for (const std::string& path : {generations_file_, generations_file_ + ".new"}) {
    auto res = aios::hal::process_host().read_file(path);
    if (!res.ok()) continue;
    any_file = true;
    std::unordered_map<std::string, uint32_t> got;
    if (!decode_generations(std::string(reinterpret_cast<const char*>(res->data()), res->size()), &got)) continue;
    if (!have_best || generation_sum(got) > generation_sum(best)) { best = std::move(got); have_best = true; }
  }
  if (have_best) { resource_generations_ = std::move(best); return; }
  if (any_file) {
    store_ok_ = false;
    aios::hal::host_note("[capability] " + generations_file_ + " is damaged or was signed with another key: its revocations are unknown, so every resource check is refused until it is restored or removed\n");
  }
}

bool CapabilityGuard::save_generations() const {
  if (generations_file_.empty()) return true;
  const std::string out = encode_generations();
  aios::ByteSpan data(reinterpret_cast<const uint8_t*>(out.data()), out.size());
  auto& host = aios::hal::process_host();
  return host.write_file(generations_file_ + ".new", data).ok() && host.write_file(generations_file_, data).ok();
}

std::pair<uint64_t, uint64_t> CapabilityGuard::hash_resource_name(const std::string& name) {
  aios::ByteSpan span{reinterpret_cast<const uint8_t*>(name.data()), name.size()};
  auto h = aios::crypto::sha256(span);
  uint64_t hi, lo;
  std::memcpy(&hi, h.data(), sizeof(uint64_t));
  std::memcpy(&lo, h.data() + 8, sizeof(uint64_t));
  return {hi, lo};
}

void CapabilityGuard::sign_token(CapabilityToken& token) const {
  // Hash token content (excluding MAC) with secret key
  size_t payload_len = sizeof(CapabilityToken) - sizeof(token.mac);
  aios::ByteSpan key_span{reinterpret_cast<const uint8_t*>(secret_.data()), secret_.size()};
  aios::ByteSpan msg_span{reinterpret_cast<const uint8_t*>(&token), payload_len};

  auto mac_full = aios::crypto::hmac_sha256(key_span, msg_span);
  std::memcpy(token.mac, mac_full.data(), sizeof(token.mac));
}

bool CapabilityGuard::verify_signature(const CapabilityToken& token) const {
  CapabilityToken copy = token;
  sign_token(copy);
  unsigned char diff = 0;   // constant time: no early exit on the first differing byte
  for (size_t k = 0; k < sizeof(token.mac); ++k) diff |= static_cast<unsigned char>(copy.mac[k] ^ token.mac[k]);
  return diff == 0;
}

CapabilityToken CapabilityGuard::mint_token(uint64_t obj_hi, uint64_t obj_lo,
                                           CapabilityRight rights,
                                           uint64_t ttl_ns) {
  CapabilityToken tok;
  tok.object_id_hi = obj_hi;
  tok.object_id_lo = obj_lo;
  tok.rights = static_cast<uint32_t>(rights);
  tok.generation = 1;

  uint64_t now_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());

  tok.expires_at_ns = (ttl_ns > 0) ? (now_ns + ttl_ns) : 0;
  sign_token(tok);
  return tok;
}

CapabilityToken CapabilityGuard::mint_for_resource(const std::string& resource_name,
                                                  CapabilityRight rights,
                                                  uint64_t ttl_ns) {
  auto [hi, lo] = hash_resource_name(resource_name);

  uint32_t gen = 1;
  {
    std::shared_lock<aios::hal::SharedMutex> lock(mutex_);
    auto it = resource_generations_.find(resource_name);
    if (it != resource_generations_.end()) {
      gen = it->second;
    }
  }

  CapabilityToken tok;
  tok.object_id_hi = hi;
  tok.object_id_lo = lo;
  tok.rights = static_cast<uint32_t>(rights);
  tok.generation = gen;

  uint64_t now_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());

  tok.expires_at_ns = (ttl_ns > 0) ? (now_ns + ttl_ns) : 0;
  sign_token(tok);
  return tok;
}

CapabilityToken CapabilityGuard::token_for_fields(const std::string& resource_name, uint32_t rights,
                                                  uint32_t generation, uint64_t expires_at_ns) const {
  auto [hi, lo] = hash_resource_name(resource_name);
  CapabilityToken tok;
  tok.object_id_hi = hi;
  tok.object_id_lo = lo;
  tok.rights = rights;
  tok.generation = generation;
  tok.expires_at_ns = expires_at_ns;
  sign_token(tok);
  return tok;
}

bool CapabilityGuard::validate_token(const CapabilityToken& token,
                                    uint64_t obj_hi, uint64_t obj_lo,
                                    CapabilityRight required_right) const {
  if (token.object_id_hi != obj_hi || token.object_id_lo != obj_lo) {
    return false;
  }

  if (!has_right(static_cast<CapabilityRight>(token.rights), required_right)) {
    return false;
  }

  if (token.expires_at_ns > 0) {
    uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    if (now_ns > token.expires_at_ns) {
      return false; // Expired
    }
  }

  return verify_signature(token);
}

bool CapabilityGuard::validate_for_resource(const CapabilityToken& token,
                                           const std::string& resource_name,
                                           CapabilityRight required_right) const {
  if (!store_ok_) return false;   // revocations unknown: refuse rather than honour a revoked token
  auto [hi, lo] = hash_resource_name(resource_name);
  if (!validate_token(token, hi, lo, required_right)) {
    return false;
  }

  std::shared_lock<aios::hal::SharedMutex> lock(mutex_);
  auto it = resource_generations_.find(resource_name);
  if (it != resource_generations_.end()) {
    if (token.generation < it->second) {
      return false; // Revoked via generation bump
    }
  }

  return true;
}

std::optional<CapabilityToken> CapabilityGuard::attenuate(const CapabilityToken& parent_token,
                                                         CapabilityRight reduced_rights,
                                                         uint64_t shorter_ttl_ns) {
  if (!verify_signature(parent_token)) {
    return std::nullopt;
  }

  // Check parent has DELEGATE right or parent rights encompass reduced rights
  uint32_t parent_r = parent_token.rights;
  uint32_t req_r = static_cast<uint32_t>(reduced_rights);
  if ((parent_r & req_r) != req_r) {
    return std::nullopt; // Cannot grant rights you do not possess
  }

  CapabilityToken child = parent_token;
  child.rights = req_r;

  uint64_t now_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());

  if (shorter_ttl_ns > 0) {
    uint64_t new_expiry = now_ns + shorter_ttl_ns;
    if (parent_token.expires_at_ns > 0) {
      child.expires_at_ns = std::min(parent_token.expires_at_ns, new_expiry);
    } else {
      child.expires_at_ns = new_expiry;
    }
  }

  sign_token(child);
  return child;
}

bool CapabilityGuard::revoke_resource(const std::string& resource_name) {
  std::unique_lock<aios::hal::SharedMutex> lock(mutex_);
  // A resource never revoked is implicitly at generation 1 (what mint_for_resource
  // stamps). The first revocation must move it past that, to 2 — inserting 0 and
  // bumping to 1 left every outstanding token valid.
  auto [it, inserted] = resource_generations_.try_emplace(resource_name, 1u);
  it->second++;
  return save_generations();
}

} // namespace aios::sidecar
