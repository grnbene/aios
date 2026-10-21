#pragma once
#include <cstdint>
#include <cstddef>

namespace aios::crypto {

// OpenBSD / OpenSSH bcrypt_pbkdf key derivation function (RFC / OpenSSH custom PBKDF2)
int bcrypt_pbkdf(const char* pass, size_t passlen, const uint8_t* salt, size_t saltlen,
                 uint8_t* key, size_t keylen, unsigned int rounds);

}  // namespace aios::crypto
