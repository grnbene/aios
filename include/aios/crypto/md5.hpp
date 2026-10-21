#pragma once
// MD5 (RFC 1321). Broken as a hash of anything an adversary chooses; it is
// here because the legacy encrypted PEM format ("Proc-Type: 4,ENCRYPTED",
// what `ssh-keygen -m PEM -N` and OpenSSL's traditional format write) derives
// its key with OpenSSL's EVP_BytesToKey over MD5, and a person's existing key
// in that form must still open.
#include <array>
#include <cstdint>

#include "aios/core/bytes.hpp"

namespace aios::crypto {

std::array<uint8_t, 16> md5(ByteSpan data);

}  // namespace aios::crypto
