// grain's message digests: sha256, sha1, sha512, sha384, md5, keccak256 and
// hmac_sha256, each over a string's bytes and answered as lowercase hex; and
// aes_cbc_decrypt and hash_equals, for reading secrets other programs stored. The
// implementations are aios's own (src/crypto), which have no dependencies, so
// these are part of the portable standard library rather than the sidecar.
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <random>
#include <algorithm>
#include <cstring>

#include "aios/core/bytes.hpp"
#include "aios/core/base64.hpp"
#include "aios/core/json.hpp"
#include "aios/crypto/aes_gcm.hpp"
#include "aios/crypto/bcrypt.hpp"
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

// random_bytes(n): returns a string of n cryptographically secure random bytes
Value bi_random_bytes(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "random_bytes")) return Value::nil();
  int64_t n = args[0].as_int();
  if (n < 0 || n > 1048576) {
    error = "random_bytes: length must be between 0 and 1048576";
    return Value::nil();
  }
  if (n == 0) return Value::string("");
  std::string out(static_cast<size_t>(n), '\0');
  int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  bool read_ok = false;
  if (fd >= 0) {
    size_t total = 0;
    while (total < static_cast<size_t>(n)) {
      ssize_t rd = ::read(fd, &out[total], static_cast<size_t>(n) - total);
      if (rd <= 0) break;
      total += rd;
    }
    ::close(fd);
    if (total == static_cast<size_t>(n)) read_ok = true;
  }
  if (!read_ok) {
    std::random_device rd;
    for (size_t i = 0; i < static_cast<size_t>(n); i += 4) {
      uint32_t r = rd();
      size_t chunk = std::min<size_t>(4, static_cast<size_t>(n) - i);
      std::memcpy(&out[i], &r, chunk);
    }
  }
  return Value::string(std::move(out));
}

// --- bcrypt ----------------------------------------------------------------

Value bi_bcrypt_hash(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "bcrypt_hash")) return Value::nil();
  int64_t cost = 10;
  if (args.size() > 1) {
    if (!want_number(args[1], error, "bcrypt_hash")) return Value::nil();
    cost = args[1].as_int();
    if (cost < 4 || cost > 31) {
      error = "bcrypt_hash: cost must be between 4 and 31";
      return Value::nil();
    }
  }

  uint8_t salt[16];
  int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  bool read_ok = false;
  if (fd >= 0) {
    size_t total = 0;
    while (total < 16) {
      ssize_t rd = ::read(fd, &salt[total], 16 - total);
      if (rd <= 0) break;
      total += rd;
    }
    ::close(fd);
    if (total == 16) read_ok = true;
  }
  if (!read_ok) {
    std::random_device rd;
    for (size_t i = 0; i < 16; i += 4) {
      uint32_t r = rd();
      std::memcpy(&salt[i], &r, 4);
    }
  }

  char out60[61];
  const std::string& pass = args[0].str();
  if (!crypto::bcrypt_hash(pass.data(), pass.size(), salt, static_cast<unsigned int>(cost), out60)) {
    error = "bcrypt_hash: failed to generate hash";
    return Value::nil();
  }
  return Value::string(out60);
}

Value bi_bcrypt_verify(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "bcrypt_verify") || !want_str(args[1], error, "bcrypt_verify")) return Value::nil();
  const std::string& pass = args[0].str();
  const std::string& hash = args[1].str();
  bool ok = crypto::bcrypt_verify(pass.data(), pass.size(), hash.c_str());
  return Value::boolean(ok);
}

// --- JWT -------------------------------------------------------------------

static std::string base64url_encode(ByteSpan data) {
  std::string s = base64_encode(data);
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '+') out += '-';
    else if (c == '/') out += '_';
    else if (c != '=') out += c;
  }
  return out;
}

static std::string base64url_decode(const std::string& in, bool& ok) {
  std::string b64;
  b64.reserve(in.size() + 4);
  for (char c : in) {
    if (c == '-') b64 += '+';
    else if (c == '_') b64 += '/';
    else b64 += c;
  }
  while (b64.size() % 4 != 0) b64 += '=';
  auto res = base64_decode(b64);
  if (!res.ok()) {
    ok = false;
    return "";
  }
  ok = true;
  return std::string(reinterpret_cast<const char*>(res->data()), res->size());
}

static Value json_to_grain(const aios::json::Value& jv) {
  switch (jv.type()) {
    case aios::json::Value::Type::Null: return Value::nil();
    case aios::json::Value::Type::Bool: return Value::boolean(jv.boolean_or(false));
    case aios::json::Value::Type::Number: {
      double d = jv.num();
      if (std::floor(d) == d && d >= (double)std::numeric_limits<int64_t>::min() && d <= (double)std::numeric_limits<int64_t>::max()) {
        return Value::integer(jv.i64());
      }
      return Value::real(d);
    }
    case aios::json::Value::Type::String: return Value::string(jv.str());
    case aios::json::Value::Type::Array: {
      std::vector<Value> list;
      list.reserve(jv.size());
      for (const auto& item : jv.items()) list.push_back(json_to_grain(item));
      return Value::list(std::move(list));
    }
    case aios::json::Value::Type::Object: {
      auto map_obj = std::make_shared<MapObj>();
      for (const auto& member : jv.members()) {
        map_obj->put(member.first, json_to_grain(member.second));
      }
      return Value::object(Type::Map, map_obj);
    }
  }
  return Value::nil();
}

static aios::json::Value grain_to_json(const Value& gv) {
  switch (gv.type) {
    case Type::Nil: return aios::json::Value::null();
    case Type::Bool: return aios::json::Value::boolean(gv.b);
    case Type::Int: return aios::json::Value::integer(gv.i);
    case Type::Real: return aios::json::Value::number(gv.d);
    case Type::Str: return aios::json::Value::string(gv.str());
    case Type::List: {
      auto arr = aios::json::Value::array();
      for (const auto& item : gv.list_items()) arr.push(grain_to_json(item));
      return arr;
    }
    case Type::Map: {
      auto obj = aios::json::Value::object();
      if (gv.obj) {
        auto m = std::dynamic_pointer_cast<MapObj>(gv.obj);
        if (m) {
          for (const auto& entry : m->entries) {
            obj.set(entry.first, grain_to_json(entry.second));
          }
        }
      }
      return obj;
    }
    default: return aios::json::Value::string(gv.to_text());
  }
}

Value bi_jwt_sign(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::Map) { error = "jwt_sign: payload must be a map"; return Value::nil(); }
  if (!want_str(args[1], error, "jwt_sign")) return Value::nil();
  const std::string& secret = args[1].str();

  aios::json::Value hdr = aios::json::Value::object();
  hdr.set("alg", aios::json::Value::string("HS256"));
  hdr.set("typ", aios::json::Value::string("JWT"));
  std::string hdr_b64 = base64url_encode(bytes_of(hdr.dump()));

  aios::json::Value payload_json = grain_to_json(args[0]);
  std::string payload_b64 = base64url_encode(bytes_of(payload_json.dump()));

  std::string signing_input = hdr_b64 + "." + payload_b64;
  auto sig = crypto::hmac_sha256(bytes_of(secret), bytes_of(signing_input));
  std::string sig_b64 = base64url_encode(ByteSpan(sig.data(), sig.size()));

  return Value::string(signing_input + "." + sig_b64);
}

Value bi_jwt_verify(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "jwt_verify") || !want_str(args[1], error, "jwt_verify")) return Value::nil();
  const std::string& token = args[0].str();
  const std::string& secret = args[1].str();

  auto out = std::make_shared<MapObj>();
  auto fail = [&](const std::string& msg) -> Value {
    out->put("ok", Value::boolean(false));
    out->put("error", Value::string(msg));
    return Value::object(Type::Map, out);
  };

  size_t dot1 = token.find('.');
  if (dot1 == std::string::npos) return fail("malformed token: missing dot");
  size_t dot2 = token.find('.', dot1 + 1);
  if (dot2 == std::string::npos || token.find('.', dot2 + 1) != std::string::npos)
    return fail("malformed token: expected exactly three parts");

  std::string hdr_b64 = token.substr(0, dot1);
  std::string payload_b64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
  std::string sig_b64 = token.substr(dot2 + 1);

  std::string signing_input = hdr_b64 + "." + payload_b64;
  auto expected_sig = crypto::hmac_sha256(bytes_of(secret), bytes_of(signing_input));
  std::string expected_b64 = base64url_encode(ByteSpan(expected_sig.data(), expected_sig.size()));

  if (sig_b64.size() != expected_b64.size()) return fail("invalid signature");
  uint8_t diff = 0;
  for (size_t i = 0; i < sig_b64.size(); ++i) diff |= (uint8_t)(sig_b64[i] ^ expected_b64[i]);
  if (diff != 0) return fail("invalid signature");

  bool ok = false;
  std::string hdr_str = base64url_decode(hdr_b64, ok);
  if (!ok) return fail("invalid header base64");
  auto parsed_hdr = aios::json::parse(hdr_str);
  if (!parsed_hdr.ok() || (*parsed_hdr)["alg"].str_or("") != "HS256")
    return fail("unsupported or missing algorithm in header");

  std::string payload_str = base64url_decode(payload_b64, ok);
  if (!ok) return fail("invalid payload base64");
  auto parsed_payload = aios::json::parse(payload_str);
  if (!parsed_payload.ok() || parsed_payload->type() != aios::json::Value::Type::Object)
    return fail("invalid payload JSON");

  int64_t now_sec = static_cast<int64_t>(std::time(nullptr));
  if (parsed_payload->has("exp")) {
    int64_t exp = (*parsed_payload)["exp"].i64();
    if (exp < now_sec) return fail("token has expired");
  }
  if (parsed_payload->has("nbf")) {
    int64_t nbf = (*parsed_payload)["nbf"].i64();
    if (nbf > now_sec) return fail("token not yet valid");
  }

  out->put("ok", Value::boolean(true));
  out->put("payload", json_to_grain(*parsed_payload));
  return Value::object(Type::Map, out);
}

}  // namespace

void install_hash_builtins(Vm& vm) {
  vm.define_native("aes_cbc_decrypt", 3, 3, bi_aes_cbc_decrypt);
  vm.define_native("hash_equals", 2, 2, bi_hash_equals);
  vm.define_native("random_bytes", 1, 1, bi_random_bytes);
  vm.define_native("sha256", 1, 1, bi_sha256);
  vm.define_native("sha1", 1, 1, bi_sha1);
  vm.define_native("sha512", 1, 1, bi_sha512);
  vm.define_native("sha384", 1, 1, bi_sha384);
  vm.define_native("md5", 1, 1, bi_md5);
  vm.define_native("keccak256", 1, 1, bi_keccak256);
  vm.define_native("hmac_sha256", 2, 2, bi_hmac_sha256);
  vm.define_native("bcrypt_hash", 1, 2, bi_bcrypt_hash);
  vm.define_native("bcrypt_verify", 2, 2, bi_bcrypt_verify);
  vm.define_native("jwt_sign", 2, 3, bi_jwt_sign);
  vm.define_native("jwt_verify", 2, 2, bi_jwt_verify);
}

}  // namespace aios::userland::grain
