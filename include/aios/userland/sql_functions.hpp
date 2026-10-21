#pragma once
// ASQL's MySQL built-in functions that stand on their own: they take and give
// plain strings, so the engine's evaluator hands them its values and wraps
// what comes back. JSON documents, AES/ENCODE, base64, CONV, COMPRESS and the
// GET_LOCK family of named locks.
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace aios::userland::sqlfn {

// --- JSON -------------------------------------------------------------------

// One argument as the evaluator has it: SQL NULL, a JSON document (what a
// JSON_* function returned), a number (its text), or a string.
struct JsonArg {
  bool null = false;
  bool json = false;
  bool numeric = false;
  std::string text;
};

// A JSON function's answer: SQL NULL, or text that is a JSON document when
// `json` is set (JSON_SET's result) and a plain value otherwise (JSON_TYPE's
// 'ARRAY'). `error` set means the statement fails with it.
struct JsonResult {
  bool null = true;
  bool json = false;
  bool numeric = false;
  std::string text;
  std::string error;
};

// True for the JSON functions json_call answers.
bool json_function(const std::string& up_name);
JsonResult json_call(const std::string& up_name, const std::vector<JsonArg>& args);

// --- encodings and ciphers ----------------------------------------------------

// MySQL's base64: a line break after every 76 characters of output.
std::string to_base64(const std::string& in);
// False for text that is not base64 (FROM_BASE64 is then NULL).
bool from_base64(const std::string& in, std::string* out);

// CONV(n, from_base, to_base). A negative from_base reads n as signed, a
// negative to_base writes a negative result with a minus sign. False when a
// base is outside 2..36.
bool conv(const std::string& n, int from_base, int to_base, std::string* out);

// AES_ENCRYPT / AES_DECRYPT in MySQL's default block_encryption_mode,
// aes-128-ecb: the key folded into 16 bytes by XOR, PKCS#7 padding. Decrypt
// is false for input that is not whole blocks or does not unpad.
std::string aes_encrypt(const std::string& data, const std::string& key);
bool aes_decrypt(const std::string& data, const std::string& key, std::string* out);

// ENCODE / DECODE: MySQL's SQL_CRYPT stream cipher seeded from the password.
std::string sql_crypt_encode(const std::string& data, const std::string& key);
std::string sql_crypt_decode(const std::string& data, const std::string& key);

// COMPRESS / UNCOMPRESS / UNCOMPRESSED_LENGTH: a four-byte little-endian
// length, then the zlib stream. The empty string stays empty.
std::string compress(const std::string& in);
bool uncompress(const std::string& in, std::string* out);
uint32_t uncompressed_length(const std::string& in);

// --- named locks (GET_LOCK) ----------------------------------------------------

// Held by a connection (its session id), any number of times over; another
// connection waits up to timeout_s for it (negative: no limit).
// 1 got it, 0 timed out.
int lock_get(const std::string& name, double timeout_s, uint64_t session);
// Waits up to timeout_s (negative: no limit) for the lock to be free or the
// session's own, without taking it; whether it is.
// The wait ends early, with -1, when *kill (the session's KILL flag) turns non-zero:
// 1 the lock is free or the session's own, 0 timed out, -1 killed.
int lock_wait_free(const std::string& name, double timeout_s, uint64_t session, const std::atomic<uint8_t>* kill = nullptr);
// Wakes every waiter so it re-reads its KILL flag (KILL sets the flag, then calls this).
void lock_wake_waiters();
// 1 released, 0 held by someone else, -1 nobody holds it (NULL).
int lock_release(const std::string& name, uint64_t session);
bool lock_is_free(const std::string& name);
// The holder's session id, 0 when free.
uint64_t lock_holder(const std::string& name);
// Every lock the session holds, each as many times as it was taken; how many
// that was.
uint64_t lock_release_all(uint64_t session);

}  // namespace aios::userland::sqlfn
