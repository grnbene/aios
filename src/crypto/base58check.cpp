#include "aios/crypto/base58check.hpp"
#include <cstring>
#include "aios/crypto/sha256.hpp"

namespace aios::crypto {

namespace {

const char* ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

int digit_of(char c) {
  for (int i = 0; i < 58; ++i) {
    if (ALPHABET[i] == c) return i;
  }
  return -1;
}

}  // namespace

std::string base58_encode(ByteSpan data) {
  std::vector<uint8_t> digits(data.size() * 138 / 100 + 1, 0);
  size_t len = 1;
  for (size_t i = 0; i < data.size(); ++i) {
    uint32_t carry = data[i];
    for (size_t j = 0; j < len; ++j) {
      carry += (uint32_t)digits[j] << 8;
      digits[j] = (uint8_t)(carry % 58);
      carry /= 58;
    }
    while (carry > 0) {
      digits[len++] = (uint8_t)(carry % 58);
      carry /= 58;
    }
  }

  std::string out;
  for (size_t i = 0; i < data.size() && data[i] == 0; ++i) out.push_back('1');
  for (size_t i = 0; i < len; ++i) out.push_back(ALPHABET[digits[len - 1 - i]]);
  return out;
}

Result<ByteBuffer> base58_decode(const std::string& text) {
  std::vector<uint8_t> bytes(text.size() * 733 / 1000 + 1, 0);
  size_t len = 1;
  for (char c : text) {
    int d = digit_of(c);
    if (d < 0) return Result<ByteBuffer>(Status::Invalid, "not a base58 character");
    uint32_t carry = (uint32_t)d;
    for (size_t j = 0; j < len; ++j) {
      carry += (uint32_t)bytes[j] * 58;
      bytes[j] = (uint8_t)(carry & 0xff);
      carry >>= 8;
    }
    while (carry > 0) {
      bytes[len++] = (uint8_t)(carry & 0xff);
      carry >>= 8;
    }
  }

  while (len > 1 && bytes[len - 1] == 0) --len;
  bool numeric_zero = (len == 1 && bytes[0] == 0);

  ByteBuffer out;
  for (size_t i = 0; i < text.size() && text[i] == '1'; ++i) out.push_back(0);
  if (!numeric_zero) {
    for (size_t i = 0; i < len; ++i) out.push_back(bytes[len - 1 - i]);
  }
  return out;
}

std::string base58check_encode(uint8_t version, ByteSpan payload) {
  ByteBuffer data;
  data.push_back(version);
  data.insert(data.end(), payload.begin(), payload.end());
  auto check = sha256d(ByteSpan(data.data(), data.size()));
  data.insert(data.end(), check.data(), check.data() + 4);
  return base58_encode(ByteSpan(data.data(), data.size()));
}

Result<ByteBuffer> base58check_decode(const std::string& text, uint8_t& version) {
  auto raw = base58_decode(text);
  if (!raw.ok()) return raw;
  if (raw->size() < 5) return Result<ByteBuffer>(Status::Invalid, "base58check payload too short");

  size_t body = raw->size() - 4;
  auto check = sha256d(ByteSpan(raw->data(), body));
  for (int i = 0; i < 4; ++i) {
    if ((*raw)[body + i] != check[(size_t)i]) {
      return Result<ByteBuffer>(Status::Corrupt, "base58check checksum mismatch");
    }
  }

  version = (*raw)[0];
  return ByteBuffer(raw->begin() + 1, raw->begin() + (long)body);
}

}  // namespace aios::crypto
