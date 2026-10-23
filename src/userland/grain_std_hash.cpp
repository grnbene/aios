// grain's message digests: sha256, sha1, sha512, sha384, md5, keccak256 and
// hmac_sha256, each over a string's bytes and answered as lowercase hex; and
// aes_cbc_decrypt and hash_equals, for reading secrets other programs stored. The
// implementations are aios's own (src/crypto), which have no dependencies, so
// these are part of the portable standard library rather than the sidecar.
#include <string>
#include <vector>

#include "aios/core/bytes.hpp"
#include "aios/crypto/aes_gcm.hpp"
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

// aes_cbc_decrypt(key, iv, data): AES-CBC (a 16, 24 or 32 byte key, a 16 byte IV) with PKCS#7 padding,
// what openssl_decrypt(..., 'AES-*-CBC', $key, OPENSSL_RAW_DATA, $iv) reads. The plaintext as a string, or
// nil when the data is not whole blocks or its padding is wrong (a wrong key, mostly). Arguments are raw bytes.
Value bi_aes_cbc_decrypt(Vm&, std::vector<Value>& args, std::string& error) {
  for (auto& a : args)
    if (!want_str(a, error, "aes_cbc_decrypt")) return Value::nil();
  const std::string &key = args[0].str(), &iv = args[1].str(), &data = args[2].str();
  if (key.size() != 16 && key.size() != 24 && key.size() != 32) { error = "aes_cbc_decrypt: the key is 16, 24 or 32 bytes"; return Value::nil(); }
  if (iv.size() != 16) { error = "aes_cbc_decrypt: the IV is 16 bytes"; return Value::nil(); }
  if (data.empty() || data.size() % 16) return Value::nil();
  std::string out(data.size(), '\0');
  if (!crypto::aes_cbc_decrypt(bytes_of(key), bytes_of(iv), bytes_of(data),
                               MutableByteSpan(reinterpret_cast<uint8_t*>(out.data()), out.size())))
    return Value::nil();
  const uint8_t n = (uint8_t)out.back();
  if (n == 0 || n > 16) return Value::nil();
  for (size_t i = out.size() - n; i < out.size(); ++i)
    if ((uint8_t)out[i] != n) return Value::nil();
  out.resize(out.size() - n);
  return Value::string(std::move(out));
}

// hash_equals(a, b): compares two strings in time that depends on their lengths only, for secrets.
Value bi_hash_equals(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "hash_equals") || !want_str(args[1], error, "hash_equals")) return Value::nil();
  const std::string &a = args[0].str(), &b = args[1].str();
  if (a.size() != b.size()) return Value::boolean(false);
  uint8_t diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff |= (uint8_t)(a[i] ^ b[i]);
  return Value::boolean(diff == 0);
}

}  // namespace

void install_hash_builtins(Vm& vm) {
  vm.define_native("aes_cbc_decrypt", 3, 3, bi_aes_cbc_decrypt);
  vm.define_native("hash_equals", 2, 2, bi_hash_equals);
  vm.define_native("sha256", 1, 1, bi_sha256);
  vm.define_native("sha1", 1, 1, bi_sha1);
  vm.define_native("sha512", 1, 1, bi_sha512);
  vm.define_native("sha384", 1, 1, bi_sha384);
  vm.define_native("md5", 1, 1, bi_md5);
  vm.define_native("keccak256", 1, 1, bi_keccak256);
  vm.define_native("hmac_sha256", 2, 2, bi_hmac_sha256);
}

}  // namespace aios::userland::grain
