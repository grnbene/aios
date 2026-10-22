// grain's message digests: sha256, sha1, sha512, sha384, md5, keccak256 and
// hmac_sha256, each over a string's bytes and answered as lowercase hex. The
// implementations are aios's own (src/crypto), which have no dependencies, so
// these are part of the portable standard library rather than the sidecar.
#include <string>
#include <vector>

#include "aios/core/bytes.hpp"
#include "aios/crypto/hmac_sha256.hpp"
#include "aios/crypto/keccak256.hpp"
#include "aios/crypto/md5.hpp"
#include "aios/crypto/sha1.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/crypto/sha512.hpp"
#include "aios/userland/grain.hpp"
#include "grain_builtins_internal.hpp"

namespace aios::userland::grain {

namespace {

ByteSpan bytes_of(const std::string& s) { return ByteSpan(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

template <size_t N>
Value hex_of(const std::array<uint8_t, N>& digest) {
  return Value::string(crypto::bytes_to_hex(ByteSpan(digest.data(), digest.size())));
}

Value bi_sha256(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sha256")) return Value::nil();
  return hex_of(crypto::sha256(bytes_of(args[0].str())));
}

Value bi_sha1(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sha1")) return Value::nil();
  return hex_of(crypto::sha1(bytes_of(args[0].str())));
}

Value bi_sha512(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sha512")) return Value::nil();
  return hex_of(crypto::sha512(bytes_of(args[0].str())));
}

Value bi_sha384(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sha384")) return Value::nil();
  return hex_of(crypto::sha384(bytes_of(args[0].str())));
}

Value bi_md5(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "md5")) return Value::nil();
  return hex_of(crypto::md5(bytes_of(args[0].str())));
}

Value bi_keccak256(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "keccak256")) return Value::nil();
  return hex_of(crypto::keccak256(bytes_of(args[0].str())));
}

// hmac_sha256(key, message)
Value bi_hmac_sha256(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "hmac_sha256") || !want_str(args[1], error, "hmac_sha256")) return Value::nil();
  return hex_of(crypto::hmac_sha256(bytes_of(args[0].str()), bytes_of(args[1].str())));
}

}  // namespace

void install_hash_builtins(Vm& vm) {
  vm.define_native("sha256", 1, 1, bi_sha256);
  vm.define_native("sha1", 1, 1, bi_sha1);
  vm.define_native("sha512", 1, 1, bi_sha512);
  vm.define_native("sha384", 1, 1, bi_sha384);
  vm.define_native("md5", 1, 1, bi_md5);
  vm.define_native("keccak256", 1, 1, bi_keccak256);
  vm.define_native("hmac_sha256", 2, 2, bi_hmac_sha256);
}

}  // namespace aios::userland::grain
