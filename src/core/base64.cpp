#include "aios/core/base64.hpp"

namespace aios {

namespace {
const char* ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int index_of(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}
}  // namespace

std::string base64_encode(ByteSpan data) {
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  size_t i = 0;
  while (i + 2 < data.size()) {
    uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
    out.push_back(ALPHABET[(v >> 18) & 63]);
    out.push_back(ALPHABET[(v >> 12) & 63]);
    out.push_back(ALPHABET[(v >> 6) & 63]);
    out.push_back(ALPHABET[v & 63]);
    i += 3;
  }
  size_t rem = data.size() - i;
  if (rem == 1) {
    uint32_t v = (uint32_t)data[i] << 16;
    out.push_back(ALPHABET[(v >> 18) & 63]);
    out.push_back(ALPHABET[(v >> 12) & 63]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
    out.push_back(ALPHABET[(v >> 18) & 63]);
    out.push_back(ALPHABET[(v >> 12) & 63]);
    out.push_back(ALPHABET[(v >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

Result<ByteBuffer> base64_decode(const std::string& text) {
  ByteBuffer out;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : text) {
    if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
    int d = index_of(c);
    if (d < 0) return Result<ByteBuffer>(Status::Invalid, "not a base64 character");
    acc = (acc << 6) | (uint32_t)d;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back((uint8_t)((acc >> bits) & 0xff));
    }
  }
  return out;
}

}  // namespace aios
