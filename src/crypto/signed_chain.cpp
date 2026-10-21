#include "aios/crypto/signed_chain.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef __wasi__
#include <fstream>
#endif

#include "aios/channel/rpc_value.hpp"
#include "aios/crypto/sha256.hpp"

namespace aios::crypto {
namespace {

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

template <size_t N>
bool from_hex(std::string_view hex, std::array<uint8_t, N>& out) {
  if (hex.size() != N * 2) return false;
  for (size_t i = 0; i < N; ++i) {
    const int hi = hex_digit(hex[i * 2]), lo = hex_digit(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

ByteSpan bytes_of(std::string_view s) { return ByteSpan(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

}  // namespace

std::string hex_lower(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string out;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) { out.push_back(d[p[i] >> 4]); out.push_back(d[p[i] & 15]); }
  return out;
}

std::string sha256_hex(std::string_view data) {
  auto h = sha256(bytes_of(data));
  return hex_lower(h.data(), h.size());
}

#ifndef __wasi__  // no files there: a WebAssembly engine signs with a seed (open_seed)
Result<void> Ed25519Signer::open(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (in) {
    in.read(reinterpret_cast<char*>(seed_.data()), seed_.size());
    if (in.gcount() != static_cast<std::streamsize>(seed_.size()))
      return Result<void>(Status::Invalid, "signing key " + path + " is not 32 bytes");
  } else {
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, seed_.data(), static_cast<ULONG>(seed_.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
      return Result<void>(Status::IoError, "cannot generate random signing key");
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Result<void>(Status::IoError, "cannot create signing key " + path);
    DWORD written = 0;
    bool ok = WriteFile(h, seed_.data(), static_cast<DWORD>(seed_.size()), &written, nullptr) && written == seed_.size();
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (!ok) return Result<void>(Status::IoError, "cannot write signing key " + path);
#else
    const int r = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (r < 0) return Result<void>(Status::IoError, "cannot read /dev/urandom");
    const ssize_t got = ::read(r, seed_.data(), seed_.size());
    ::close(r);
    if (got != static_cast<ssize_t>(seed_.size())) return Result<void>(Status::IoError, "short read from /dev/urandom");
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return Result<void>(Status::IoError, "cannot create signing key " + path);
    bool ok = ::write(fd, seed_.data(), seed_.size()) == static_cast<ssize_t>(seed_.size());
    ok = ::fsync(fd) == 0 && ok;
    ::close(fd);
    if (!ok) return Result<void>(Status::IoError, "cannot write signing key " + path);
#endif
  }
  pub_ = ed25519_public_from_seed(seed_);
  ready_ = true;
  return Ok();
}
#endif

Result<void> Ed25519Signer::open_seed(const Scalar32& seed) {
  seed_ = seed;
  pub_ = ed25519_public_from_seed(seed_);
  ready_ = true;
  return Ok();
}

Result<Scalar32> Ed25519Signer::seed_from_hex(std::string_view hex) {
  if (hex.size() != 64) return Result<Scalar32>(Status::Invalid, "expected 64 hex characters, got " + std::to_string(hex.size()));
  Scalar32 seed{};
  if (!from_hex(hex, seed)) return Result<Scalar32>(Status::Invalid, "not a hex string");
  return seed;
}

std::string Ed25519Signer::public_hex() const { return ready_ ? hex_lower(pub_.data(), pub_.size()) : ""; }

std::string Ed25519Signer::sign_hex(std::string_view message) const {
  const Sig64 sig = ed25519_sign(bytes_of(message), seed_, pub_);
  return hex_lower(sig.data(), sig.size());
}

bool ed25519_verify_hex(std::string_view message, std::string_view signature_hex, std::string_view public_hex) {
  Sig64 sig{};
  Point32 pub{};
  if (!from_hex(signature_hex, sig) || !from_hex(public_hex, pub)) return false;
  return ed25519_verify(bytes_of(message), sig, pub);
}

ChainCheck verify_chain(const std::vector<ChainLink>& links, std::string_view public_hex, uint64_t first_seq,
                        std::string_view first_prev) {
  ChainCheck out;
  std::string prev(first_prev);
  for (size_t i = 0; i < links.size(); ++i) {
    const auto& l = links[i];
    const uint64_t want = first_seq + i;
    const std::string at = "record " + std::to_string(want);
    ++out.links;
    if (!ed25519_verify_hex(l.record, l.signature, public_hex)) out.problems.push_back(at + ": signature does not verify");
    auto parsed = channel::rpc_parse(l.record);
    if (!parsed.ok() || parsed->kind() != channel::RpcValue::Kind::Object) {
      out.problems.push_back(at + ": not a JSON object");
    } else {
      const uint64_t seq = parsed->has("seq") ? (*parsed)["seq"].as_u64(0) : 0;
      if (seq != want) out.problems.push_back(at + ": carries seq " + std::to_string(seq) + " (a record is missing or reordered)");
      const std::string p = parsed->has("prev") ? (*parsed)["prev"].as_text() : std::string("<none>");
      if (p != prev) out.problems.push_back(at + ": does not name the previous record's hash");
    }
    prev = sha256_hex(l.record);
  }
  return out;
}

}  // namespace aios::crypto
