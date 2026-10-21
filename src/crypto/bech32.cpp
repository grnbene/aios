#include "aios/crypto/bech32.hpp"

namespace aios::crypto {

namespace {

const char* CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
const uint32_t BECH32_CONST = 1;
const uint32_t BECH32M_CONST = 0x2bc830a3;

uint32_t polymod(const ByteBuffer& values) {
  static const uint32_t GEN[5] = {0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3};
  uint32_t chk = 1;
  for (uint8_t v : values) {
    uint32_t top = chk >> 25;
    chk = ((chk & 0x1ffffff) << 5) ^ v;
    for (int i = 0; i < 5; ++i) {
      if ((top >> i) & 1) chk ^= GEN[i];
    }
  }
  return chk;
}

ByteBuffer hrp_expand(const std::string& hrp) {
  ByteBuffer out;
  out.reserve(hrp.size() * 2 + 1);
  for (char c : hrp) out.push_back((uint8_t)((uint8_t)c >> 5));
  out.push_back(0);
  for (char c : hrp) out.push_back((uint8_t)((uint8_t)c & 31));
  return out;
}

int charset_index(char c) {
  for (int i = 0; i < 32; ++i) {
    if (CHARSET[i] == c) return i;
  }
  return -1;
}

bool convert_bits(const uint8_t* in, size_t len, int from, int to, bool pad, ByteBuffer& out) {
  uint32_t acc = 0;
  int bits = 0;
  const uint32_t maxv = (uint32_t)((1 << to) - 1);
  for (size_t i = 0; i < len; ++i) {
    acc = (acc << from) | in[i];
    bits += from;
    while (bits >= to) {
      bits -= to;
      out.push_back((uint8_t)((acc >> bits) & maxv));
    }
  }
  if (pad) {
    if (bits > 0) out.push_back((uint8_t)((acc << (to - bits)) & maxv));
  } else if (bits >= from || ((acc << (to - bits)) & maxv)) {
    return false;
  }
  return true;
}

}  // namespace

std::string bech32_encode(const std::string& hrp, ByteSpan data5, Bech32Variant variant) {
  ByteBuffer values = hrp_expand(hrp);
  values.insert(values.end(), data5.begin(), data5.end());
  for (int i = 0; i < 6; ++i) values.push_back(0);

  uint32_t constant = (variant == Bech32Variant::Bech32) ? BECH32_CONST : BECH32M_CONST;
  uint32_t mod = polymod(values) ^ constant;

  std::string out = hrp + "1";
  for (uint8_t v : data5) out.push_back(CHARSET[v]);
  for (int i = 0; i < 6; ++i) out.push_back(CHARSET[(mod >> (5 * (5 - i))) & 31]);
  return out;
}

Result<ByteBuffer> bech32_decode(const std::string& text, std::string& hrp, Bech32Variant& variant) {
  if (text.size() < 8 || text.size() > 90) {
    return Result<ByteBuffer>(Status::Invalid, "bech32 length out of range");
  }

  bool has_lower = false, has_upper = false;
  for (char c : text) {
    if (c >= 'a' && c <= 'z') has_lower = true;
    if (c >= 'A' && c <= 'Z') has_upper = true;
    if ((uint8_t)c < 33 || (uint8_t)c > 126) {
      return Result<ByteBuffer>(Status::Invalid, "bech32 character out of range");
    }
  }
  if (has_lower && has_upper) return Result<ByteBuffer>(Status::Invalid, "bech32 mixed case");

  std::string lower;
  lower.reserve(text.size());
  for (char c : text) lower.push_back((c >= 'A' && c <= 'Z') ? (char)(c + 32) : c);

  size_t sep = lower.rfind('1');
  if (sep == std::string::npos || sep == 0 || sep + 7 > lower.size()) {
    return Result<ByteBuffer>(Status::Invalid, "bech32 separator misplaced");
  }

  hrp = lower.substr(0, sep);
  ByteBuffer data;
  for (size_t i = sep + 1; i < lower.size(); ++i) {
    int d = charset_index(lower[i]);
    if (d < 0) return Result<ByteBuffer>(Status::Invalid, "bech32 data character invalid");
    data.push_back((uint8_t)d);
  }

  ByteBuffer values = hrp_expand(hrp);
  values.insert(values.end(), data.begin(), data.end());
  uint32_t mod = polymod(values);
  if (mod == BECH32_CONST) {
    variant = Bech32Variant::Bech32;
  } else if (mod == BECH32M_CONST) {
    variant = Bech32Variant::Bech32m;
  } else {
    return Result<ByteBuffer>(Status::Corrupt, "bech32 checksum mismatch");
  }

  return ByteBuffer(data.begin(), data.end() - 6);
}

std::string segwit_encode(const std::string& hrp, uint8_t version, ByteSpan program) {
  ByteBuffer data5;
  data5.push_back(version);
  convert_bits(program.data(), program.size(), 8, 5, true, data5);
  auto variant = (version == 0) ? Bech32Variant::Bech32 : Bech32Variant::Bech32m;
  return bech32_encode(hrp, ByteSpan(data5.data(), data5.size()), variant);
}

Result<ByteBuffer> segwit_decode(const std::string& text, const std::string& expected_hrp, uint8_t& version) {
  std::string hrp;
  Bech32Variant variant = Bech32Variant::Bech32;
  auto data = bech32_decode(text, hrp, variant);
  if (!data.ok()) return data;
  if (hrp != expected_hrp) return Result<ByteBuffer>(Status::Invalid, "segwit prefix mismatch");
  if (data->empty()) return Result<ByteBuffer>(Status::Invalid, "segwit payload empty");

  version = (*data)[0];
  if (version > 16) return Result<ByteBuffer>(Status::Invalid, "segwit version out of range");
  auto expected = (version == 0) ? Bech32Variant::Bech32 : Bech32Variant::Bech32m;
  if (variant != expected) return Result<ByteBuffer>(Status::Invalid, "segwit checksum variant mismatch");

  ByteBuffer program;
  if (!convert_bits(data->data() + 1, data->size() - 1, 5, 8, false, program)) {
    return Result<ByteBuffer>(Status::Invalid, "segwit padding invalid");
  }
  if (program.size() < 2 || program.size() > 40) {
    return Result<ByteBuffer>(Status::Invalid, "segwit program length invalid");
  }
  if (version == 0 && program.size() != 20 && program.size() != 32) {
    return Result<ByteBuffer>(Status::Invalid, "segwit v0 program must be 20 or 32 bytes");
  }
  return program;
}

}  // namespace aios::crypto
