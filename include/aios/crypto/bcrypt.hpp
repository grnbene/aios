#pragma once
#include <cstdint>
#include <cstddef>

namespace aios::crypto {

// OpenBSD / OpenSSH bcrypt_pbkdf key derivation function (RFC / OpenSSH custom PBKDF2)
int bcrypt_pbkdf(const char* pass, size_t passlen, const uint8_t* salt, size_t saltlen,
                 uint8_t* key, size_t keylen, unsigned int rounds);

// Standard OpenBSD bcrypt password hashing ($2b$ format)
// Generates a 60-character bcrypt hash string (plus null terminator)
bool bcrypt_hash(const char* pass, size_t passlen, const uint8_t salt16[16],
                 unsigned int cost, char out60[61]);

// Verifies a password against a standard bcrypt hash ($2a$ or $2b$) in constant time
bool bcrypt_verify(const char* pass, size_t passlen, const char* hash);

}  // namespace aios::crypto
