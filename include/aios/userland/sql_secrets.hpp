#pragma once
// Secrets for ASQL: named values (a bot token, an API key) kept sealed on disk and opened only at the moment
// something needs them, so the token never sits in a declaration, a SHOW, the query log or a backup in plain text.
//
//   CREATE SECRET tg_token AS '123456:ABC...'    seal a value under a name (replaces one of that name)
//   DROP SECRET tg_token
//   SHOW SECRETS                                 names and when they were set; never a value
//
// A Cells NOTIFY names one as {{secret:tg_token}} in its URL, headers or body (sql_cells.hpp).
//
// Each value is AES-256-GCM sealed under a random 96-bit nonce, with the secret's name as the associated data, so a
// sealed value moved under another name does not open. The key is 32 random bytes in <store>/secrets/master.key,
// created readable by this account only on first use; AIOS_SECRET_KEY_FILE names another file (64 hex characters or
// 32 raw bytes) to keep the key off the data volume. Sealed values are in <store>/secrets/secrets.db. Whoever can
// read both files can read the secrets: the key file is what to keep out of backups that carry the data.
#include <mutex>
#include <string>
#include <vector>

namespace aios::hal { class Host; }

namespace aios::userland {

class SecretStore {
 public:
  struct Entry {
    std::string name;
    std::string set_at;   // local time it was sealed
  };

  SecretStore(aios::hal::Host& host, std::string dir) : host_(host), dir_(std::move(dir)) {}

  // False with `err` set when the name is not letters, digits and _, the value is empty, or the store cannot be written.
  bool set(const std::string& name, const std::string& value, std::string* err);
  bool drop(const std::string& name, std::string* err);
  bool has(const std::string& name);
  // The opened value; false when there is no such secret or its seal does not verify.
  bool get(const std::string& name, std::string* value, std::string* err);
  std::vector<Entry> list();
  // `text` with every secret's value replaced by ***: what an error message goes through before it leaves.
  std::string scrub(std::string text);

  // The statement as the query log may keep it: CREATE SECRET <name> AS '***'. Anything else comes back unchanged.
  static std::string redact(const std::string& sql);

 private:
  struct Item {
    std::string name, at;
    std::vector<unsigned char> sealed;   // nonce || ciphertext || tag
  };
  bool load_locked(std::string* err);
  bool save_locked(std::string* err);
  bool key_locked(unsigned char out[32], std::string* err);

  aios::hal::Host& host_;
  std::string dir_;
  std::mutex mu_;
  bool loaded_ = false;
  std::vector<Item> items_;
};

}  // namespace aios::userland
