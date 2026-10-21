// ASQL capability credentials: a bearer string that is the whole of a client's
// authority over the database. There are no accounts behind it - what a
// credential can touch, do, and for how long is written in it and signed with
// the machine's capability key (aios::sidecar::CapabilityGuard).
//
//   asql1.<hex spec>.<hex fields>.<hex caveats>.<hex tag>
//
// spec    "asql:<id>:<db>.<table>,<db>.*"  the root grant; <table> may be * for a whole database
// fields  rights (u32), generation (u32), expiry in ns (u64); 0 expiry is none
// caveats newline-separated restrictions the holder appended without the key
// tag     16-byte chain MAC; the guard's own MAC of spec+fields, then one
//         HMAC step per caveat, so a caveat can be added and never removed
//
// Verification needs the key; attenuation does not.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aios/hal/sync.hpp"

namespace aios::sidecar { class CapabilityGuard; }

namespace aios::userland {

struct AsqlScopeEntry {
  std::string db;     // lower case
  std::string table;  // lower case, or "*"
};

// What a verified credential permits, as the engine's statement gate reads it.
struct AsqlAuthority {
  uint32_t rights = 0;                    // aios::sidecar::CapabilityRight bits
  std::vector<AsqlScopeEntry> scopes;
  uint64_t expires_at_ns = 0;             // 0 = never
  uint64_t rows_cap = 0;                  // 0 = unlimited; rows returned or changed, over the session
  std::string spec;                       // the root grant, the unit a revocation names

  bool covers(const std::string& db, const std::string& table) const;
  bool covers_whole_db(const std::string& db) const;
  bool names_db(const std::string& db) const;
};

struct AsqlGrant {
  std::vector<AsqlScopeEntry> scopes;
  uint32_t rights = 0;
  uint64_t ttl_ns = 0;                    // 0 = no expiry
  uint64_t rows_cap = 0;
};

// "db.table,db.*" -> entries; false with `err` on anything malformed.
bool asql_parse_scopes(const std::string& text, std::vector<AsqlScopeEntry>* out, std::string* err);
// "read,write,delete,admin,execute" -> rights bits.
bool asql_parse_rights(const std::string& text, uint32_t* out, std::string* err);
std::string asql_rights_text(uint32_t rights);

// Signs a new root credential. Needs the guard (the key).
bool asql_mint(sidecar::CapabilityGuard& guard, const AsqlGrant& grant, std::string* credential, std::string* err);

// Narrows a credential: the result can do no more than the input and
// possibly less. Needs no key; the guard is not involved.
bool asql_attenuate(const std::string& credential, const AsqlGrant& narrower, std::string* out, std::string* err);

// Checks the chain, the expiry and the revocation, and folds the caveats into
// one authority. False with `err` for anything but a credential that is valid now.
bool asql_verify(const sidecar::CapabilityGuard& guard, const std::string& credential,
                 AsqlAuthority* out, std::string* err);

// Revokes the root grant the credential descends from, and with it every
// credential attenuated from that root.
bool asql_revoke(sidecar::CapabilityGuard& guard, const std::string& credential, std::string* err);
// What is left of a credential's lifetime, for a grant that re-issues it with the same expiry (0 = it never expires).
uint64_t asql_remaining_ttl_ns(const AsqlAuthority& a);

// The machine's capability key: AIOS_CAPABILITY_KEY, else <root>/capability.key
// (made, 0600, with 32 random bytes on first use). Empty on failure.
std::string asql_load_capability_key(const std::string& root_dir);

// A named user: a login name and a password standing in front of a credential,
// so a person or a program connects as `-u name -p<password>` from any MySQL
// client instead of pasting the credential through mysql_clear_password. The
// session that results holds the credential's authority, nothing more.
struct AsqlUser {
  std::string name;
  std::string password_hash;  // hex SHA1(SHA1(password)), the mysql_native_password form
  std::string credential;
};

// <root>/capability-users: one user per line, name TAB hash TAB credential.
// Rewritten whole through a temporary file, and read again by whoever holds
// it whenever the file changed, so the server and `aios asqlcap user` share it.
class AsqlUserStore {
 public:
  explicit AsqlUserStore(std::string path);
  bool find(const std::string& name, AsqlUser* out);
  std::vector<AsqlUser> list();
  bool put(const AsqlUser& user, std::string* err);
  bool erase(const std::string& name, std::string* err);
  const std::string& path() const { return path_; }

 private:
  bool reload_locked();
  bool save_locked(std::string* err);
  std::string path_;
  std::vector<AsqlUser> users_;
  int64_t seen_mtime_ = -1;
  int64_t seen_size_ = -1;
  mutable hal::SharedMutex mutex_;
};

bool asql_valid_user_name(const std::string& name);
std::string asql_password_hash(const std::string& password);
// A mysql_native_password reply (20 bytes) against a stored hash and the 20-byte salt.
bool asql_password_matches(const std::string& password_hash, const uint8_t* salt, const std::vector<uint8_t>& response);

}  // namespace aios::userland
