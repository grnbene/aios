#include "aios/userland/asql_capability.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sys/stat.h>

#include "aios/crypto/hmac_sha256.hpp"
#include "aios/crypto/sha1.hpp"
#include "aios/sidecar/capability_guard.hpp"

namespace aios::userland {

namespace {

using sidecar::CapabilityRight;

constexpr uint32_t kRightBits[] = {1u << 0, 1u << 1, 1u << 2, 1u << 3, 1u << 4, 1u << 5};
constexpr const char* kRightNames[] = {"read", "write", "execute", "delete", "delegate", "admin"};

std::string lower(std::string s) {
  for (char& c : s) c = (char)::tolower((unsigned char)c);
  return s;
}

std::string hex_of(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string o;
  o.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) { o += d[p[i] >> 4]; o += d[p[i] & 15]; }
  return o;
}
std::string hex_of(const std::string& s) { return hex_of(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

bool unhex(const std::string& in, std::string* out) {
  if (in.size() % 2) return false;
  auto nib = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
  out->clear();
  for (size_t i = 0; i < in.size(); i += 2) {
    int h = nib(in[i]), l = nib(in[i + 1]);
    if (h < 0 || l < 0) return false;
    out->push_back((char)(h * 16 + l));
  }
  return true;
}

uint64_t now_ns() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string scopes_text(const std::vector<AsqlScopeEntry>& scopes) {
  std::string o;
  for (const auto& s : scopes) o += (o.empty() ? "" : ",") + s.db + "." + s.table;
  return o;
}

bool entry_covers(const AsqlScopeEntry& a, const AsqlScopeEntry& b) {
  return (a.db == "*" || a.db == b.db) && (a.table == "*" || a.table == b.table);   // *.* is every database
}

struct Parsed {
  std::string spec;
  uint32_t rights = 0, generation = 0;
  uint64_t expires = 0;
  std::vector<std::string> caveats;
  uint8_t tag[16] = {0};
};

bool split_dots(const std::string& s, std::vector<std::string>* parts) {
  size_t at = 0;
  for (;;) {
    size_t dot = s.find('.', at);
    parts->push_back(s.substr(at, dot == std::string::npos ? std::string::npos : dot - at));
    if (dot == std::string::npos) return true;
    at = dot + 1;
  }
}

bool parse_credential(const std::string& text, Parsed* p, std::string* err) {
  std::string t = text;
  while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
  std::vector<std::string> parts;
  split_dots(t, &parts);
  if (parts.size() != 5 || parts[0] != "asql1") { *err = "not an asql1 credential"; return false; }
  std::string fields, caveats, tag;
  if (!unhex(parts[1], &p->spec) || !unhex(parts[2], &fields) || !unhex(parts[3], &caveats) || !unhex(parts[4], &tag) ||
      fields.size() != 16 || tag.size() != 16) {
    *err = "malformed credential";
    return false;
  }
  std::memcpy(&p->rights, fields.data(), 4);
  std::memcpy(&p->generation, fields.data() + 4, 4);
  std::memcpy(&p->expires, fields.data() + 8, 8);
  std::memcpy(p->tag, tag.data(), 16);
  size_t at = 0;
  while (at < caveats.size()) {
    size_t nl = caveats.find('\n', at);
    if (nl == std::string::npos) nl = caveats.size();
    p->caveats.push_back(caveats.substr(at, nl - at));
    at = nl + 1;
  }
  return true;
}

void chain_step(uint8_t tag[16], const std::string& caveat) {
  ByteSpan key{tag, 16};
  ByteSpan msg{reinterpret_cast<const uint8_t*>(caveat.data()), caveat.size()};
  auto h = crypto::hmac_sha256(key, msg);
  std::memcpy(tag, h.data(), 16);
}

std::string assemble(const Parsed& p) {
  std::string fields(16, '\0');
  std::memcpy(&fields[0], &p.rights, 4);
  std::memcpy(&fields[4], &p.generation, 4);
  std::memcpy(&fields[8], &p.expires, 8);
  std::string cav;
  for (size_t i = 0; i < p.caveats.size(); ++i) cav += (i ? "\n" : "") + p.caveats[i];
  return "asql1." + hex_of(p.spec) + "." + hex_of(fields) + "." + hex_of(cav) + "." + hex_of(p.tag, 16);
}

bool parse_u64(const std::string& s, uint64_t* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  *out = std::strtoull(s.c_str(), &end, 10);
  return end && *end == '\0';
}

}  // namespace

bool AsqlAuthority::covers(const std::string& db, const std::string& table) const {
  for (const auto& s : scopes)
    if ((s.db == "*" || s.db == db) && (s.table == "*" || s.table == table)) return true;
  return false;
}
bool AsqlAuthority::covers_whole_db(const std::string& db) const {
  for (const auto& s : scopes)
    if ((s.db == "*" || s.db == db) && s.table == "*") return true;
  return false;
}
bool AsqlAuthority::names_db(const std::string& db) const {
  for (const auto& s : scopes)
    if (s.db == "*" || s.db == db) return true;
  return false;
}

bool asql_parse_scopes(const std::string& text, std::vector<AsqlScopeEntry>* out, std::string* err) {
  out->clear();
  size_t at = 0;
  while (at <= text.size()) {
    size_t comma = text.find(',', at);
    std::string item = text.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
    at = comma == std::string::npos ? text.size() + 1 : comma + 1;
    while (!item.empty() && item.front() == ' ') item.erase(item.begin());
    while (!item.empty() && item.back() == ' ') item.pop_back();
    if (item.empty()) continue;
    const size_t dot = item.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == item.size() || item.find('.', dot + 1) != std::string::npos) {
      *err = "scope '" + item + "' is not db.table or db.*";
      return false;
    }
    AsqlScopeEntry e{lower(item.substr(0, dot)), lower(item.substr(dot + 1))};
    for (const std::string* part : {&e.db, &e.table})
      for (char c : *part)
        if (!(std::isalnum((unsigned char)c) || c == '_' || c == '$' || c == '*' || c == '-')) {
          *err = "scope '" + item + "' has a character a name cannot";
          return false;
        }
    if ((e.db.find('*') != std::string::npos && !(e.db == "*" && e.table == "*")) ||
        (e.table.find('*') != std::string::npos && e.table != "*")) {
      *err = "scope '" + item + "': * may only stand for a whole table list";
      return false;
    }
    out->push_back(std::move(e));
  }
  if (out->empty()) { *err = "no scope given"; return false; }
  return true;
}

bool asql_parse_rights(const std::string& text, uint32_t* out, std::string* err) {
  *out = 0;
  size_t at = 0;
  while (at <= text.size()) {
    size_t comma = text.find(',', at);
    std::string item = lower(text.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
    at = comma == std::string::npos ? text.size() + 1 : comma + 1;
    if (item.empty()) continue;
    bool found = false;
    for (size_t i = 0; i < 6; ++i)
      if (item == kRightNames[i]) { *out |= kRightBits[i]; found = true; }
    if (!found) { *err = "unknown right '" + item + "' (read, write, execute, delete, delegate, admin)"; return false; }
  }
  if (*out == 0) { *err = "no rights given"; return false; }
  return true;
}

std::string asql_rights_text(uint32_t rights) {
  std::string o;
  for (size_t i = 0; i < 6; ++i)
    if (rights & kRightBits[i]) o += (o.empty() ? "" : ",") + std::string(kRightNames[i]);
  return o.empty() ? "none" : o;
}

bool asql_mint(sidecar::CapabilityGuard& guard, const AsqlGrant& grant, std::string* credential, std::string* err) {
  if (grant.scopes.empty() || grant.rights == 0) { *err = "a grant needs a scope and a right"; return false; }
  if (!guard.revocation_store_ok()) { *err = "the revocation store is damaged; refusing to mint"; return false; }
  std::random_device rd;
  uint8_t id[8];
  for (auto& b : id) b = (uint8_t)rd();
  Parsed p;
  p.spec = "asql:" + hex_of(id, 8) + ":" + scopes_text(grant.scopes);
  auto tok = guard.mint_for_resource(p.spec, (CapabilityRight)grant.rights, grant.ttl_ns);
  p.rights = tok.rights;
  p.generation = tok.generation;
  p.expires = tok.expires_at_ns;
  std::memcpy(p.tag, tok.mac, 16);
  if (grant.rows_cap) {
    const std::string c = "rows=" + std::to_string(grant.rows_cap);
    p.caveats.push_back(c);
    chain_step(p.tag, c);
  }
  *credential = assemble(p);
  return true;
}

bool asql_attenuate(const std::string& credential, const AsqlGrant& narrower, std::string* out, std::string* err) {
  Parsed p;
  if (!parse_credential(credential, &p, err)) return false;
  auto add = [&](const std::string& c) { p.caveats.push_back(c); chain_step(p.tag, c); };
  if (!narrower.scopes.empty()) add("tables=" + scopes_text(narrower.scopes));
  if (narrower.rights) add("rights=" + std::to_string(narrower.rights));
  if (narrower.ttl_ns) add("expires=" + std::to_string(now_ns() + narrower.ttl_ns));
  if (narrower.rows_cap) add("rows=" + std::to_string(narrower.rows_cap));
  *out = assemble(p);
  return true;
}

bool asql_verify(const sidecar::CapabilityGuard& guard, const std::string& credential,
                 AsqlAuthority* out, std::string* err) {
  Parsed p;
  if (!parse_credential(credential, &p, err)) return false;
  if (p.spec.rfind("asql:", 0) != 0) { *err = "not an ASQL grant"; return false; }

  const auto root = guard.token_for_fields(p.spec, p.rights, p.generation, p.expires);
  uint8_t tag[16];
  std::memcpy(tag, root.mac, 16);
  for (const auto& c : p.caveats) chain_step(tag, c);
  unsigned char diff = 0;
  for (size_t i = 0; i < 16; ++i) diff |= (unsigned char)(tag[i] ^ p.tag[i]);
  if (diff != 0) { *err = "credential signature does not verify"; return false; }
  if (!guard.validate_for_resource(root, p.spec, CapabilityRight::NONE)) {
    *err = "credential is expired or revoked";
    return false;
  }

  AsqlAuthority a;
  a.spec = p.spec;
  a.rights = p.rights;
  a.expires_at_ns = p.expires;
  const size_t scope_at = p.spec.find(':', 5);
  std::string serr;
  if (scope_at == std::string::npos || !asql_parse_scopes(p.spec.substr(scope_at + 1), &a.scopes, &serr)) {
    *err = "grant names no valid scope";
    return false;
  }
  for (const auto& c : p.caveats) {
    const size_t eq = c.find('=');
    const std::string key = eq == std::string::npos ? c : c.substr(0, eq);
    const std::string val = eq == std::string::npos ? "" : c.substr(eq + 1);
    uint64_t n = 0;
    if (key == "tables") {
      std::vector<AsqlScopeEntry> want, kept;
      if (!asql_parse_scopes(val, &want, &serr)) { *err = "malformed caveat"; return false; }
      for (const auto& w : want)
        for (const auto& have : a.scopes) {
          const AsqlScopeEntry* pick = entry_covers(have, w) ? &w : entry_covers(w, have) ? &have : nullptr;
          if (pick && std::none_of(kept.begin(), kept.end(), [&](const AsqlScopeEntry& k) { return k.db == pick->db && k.table == pick->table; }))
            kept.push_back(*pick);
        }
      a.scopes = std::move(kept);
    } else if (key == "rights" && parse_u64(val, &n)) {
      a.rights &= (uint32_t)n;
    } else if (key == "expires" && parse_u64(val, &n)) {
      a.expires_at_ns = a.expires_at_ns ? std::min(a.expires_at_ns, n) : n;
    } else if (key == "rows" && parse_u64(val, &n)) {
      a.rows_cap = a.rows_cap ? std::min(a.rows_cap, n) : n;
    } else {
      *err = "unknown caveat '" + key + "'";
      return false;
    }
  }
  if (a.expires_at_ns && now_ns() > a.expires_at_ns) { *err = "credential is expired or revoked"; return false; }
  *out = std::move(a);
  return true;
}

bool asql_revoke(sidecar::CapabilityGuard& guard, const std::string& credential, std::string* err) {
  AsqlAuthority a;
  if (!asql_verify(guard, credential, &a, err)) return false;
  if (!guard.revoke_resource(a.spec)) { *err = "revoked in memory, but the revocation store could not be written"; return false; }
  return true;
}

uint64_t asql_remaining_ttl_ns(const AsqlAuthority& a) {
  if (!a.expires_at_ns) return 0;
  const uint64_t n = now_ns();
  return a.expires_at_ns > n ? a.expires_at_ns - n : 1;
}

bool asql_valid_user_name(const std::string& name) {
  if (name.empty() || name.size() > 32 || name == "cap") return false;
  for (char c : name)
    if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.')) return false;
  return true;
}

std::string asql_password_hash(const std::string& password) {
  const auto s1 = crypto::sha1(ByteSpan{reinterpret_cast<const uint8_t*>(password.data()), password.size()});
  const auto s2 = crypto::sha1(ByteSpan{s1.data(), s1.size()});
  return hex_of(s2.data(), s2.size());
}

// The client sends SHA1(pw) XOR SHA1(salt + SHA1(SHA1(pw))). Knowing only the
// double hash, undo the XOR and check that what is left hashes to it.
bool asql_password_matches(const std::string& password_hash, const uint8_t* salt, const std::vector<uint8_t>& response) {
  std::string s2;
  if (response.size() != 20 || !unhex(password_hash, &s2) || s2.size() != 20) return false;
  uint8_t seed[40];
  std::memcpy(seed, salt, 20);
  std::memcpy(seed + 20, s2.data(), 20);
  const auto shake = crypto::sha1(ByteSpan{seed, sizeof seed});
  uint8_t s1[20];
  for (size_t i = 0; i < 20; ++i) s1[i] = (uint8_t)(response[i] ^ shake[i]);
  const auto check = crypto::sha1(ByteSpan{s1, sizeof s1});
  unsigned diff = 0;
  for (size_t i = 0; i < 20; ++i) diff |= (unsigned)(check[i] ^ (uint8_t)s2[i]);
  return diff == 0;
}

AsqlUserStore::AsqlUserStore(std::string path) : path_(std::move(path)) {}

bool AsqlUserStore::reload_locked() {
  struct stat st{};
  if (::stat(path_.c_str(), &st) != 0) {
    users_.clear();
    seen_mtime_ = seen_size_ = -1;
    return true;
  }
  if ((int64_t)st.st_mtime == seen_mtime_ && (int64_t)st.st_size == seen_size_) return true;
  std::ifstream in(path_);
  if (!in) return false;
  std::vector<AsqlUser> fresh;
  std::string line;
  while (std::getline(in, line)) {
    const size_t t1 = line.find('\t');
    const size_t t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
    if (t2 == std::string::npos) continue;
    fresh.push_back({line.substr(0, t1), line.substr(t1 + 1, t2 - t1 - 1), line.substr(t2 + 1)});
  }
  users_ = std::move(fresh);
  seen_mtime_ = (int64_t)st.st_mtime;
  seen_size_ = (int64_t)st.st_size;
  return true;
}

bool AsqlUserStore::save_locked(std::string* err) {
  const std::string tmp = path_ + ".tmp";
  const mode_t old_mask = ::umask(077);
  {
    std::ofstream out(tmp, std::ios::trunc);
    for (const auto& u : users_) out << u.name << '\t' << u.password_hash << '\t' << u.credential << '\n';
    out.close();
    if (out.fail()) {
      ::umask(old_mask);
      *err = "cannot write " + tmp;
      return false;
    }
  }
  ::umask(old_mask);
  if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
    *err = "cannot replace " + path_;
    return false;
  }
  struct stat st{};
  if (::stat(path_.c_str(), &st) == 0) {
    seen_mtime_ = (int64_t)st.st_mtime;
    seen_size_ = (int64_t)st.st_size;
  }
  return true;
}

bool AsqlUserStore::find(const std::string& name, AsqlUser* out) {
  std::unique_lock<hal::SharedMutex> lock(mutex_);
  if (!reload_locked()) return false;
  for (const auto& u : users_)
    if (u.name == name) { *out = u; return true; }
  return false;
}

std::vector<AsqlUser> AsqlUserStore::list() {
  std::unique_lock<hal::SharedMutex> lock(mutex_);
  reload_locked();
  return users_;
}

bool AsqlUserStore::put(const AsqlUser& user, std::string* err) {
  std::unique_lock<hal::SharedMutex> lock(mutex_);
  if (!reload_locked()) { *err = "cannot read " + path_; return false; }
  bool replaced = false;
  for (auto& u : users_)
    if (u.name == user.name) { u = user; replaced = true; }
  if (!replaced) users_.push_back(user);
  return save_locked(err);
}

bool AsqlUserStore::erase(const std::string& name, std::string* err) {
  std::unique_lock<hal::SharedMutex> lock(mutex_);
  if (!reload_locked()) { *err = "cannot read " + path_; return false; }
  const size_t before = users_.size();
  users_.erase(std::remove_if(users_.begin(), users_.end(), [&](const AsqlUser& u) { return u.name == name; }), users_.end());
  if (users_.size() == before) { *err = "no user '" + name + "'"; return false; }
  return save_locked(err);
}

std::string asql_load_capability_key(const std::string& root_dir) {
  if (const char* env = std::getenv("AIOS_CAPABILITY_KEY"); env && *env) return env;
  const std::string path = root_dir + "/capability.key";
  {
    std::ifstream in(path);
    std::string key;
    if (in && std::getline(in, key) && !key.empty()) return key;
  }
  uint8_t raw[32];
  std::ifstream rnd("/dev/urandom", std::ios::binary);
  if (!rnd.read(reinterpret_cast<char*>(raw), sizeof raw)) return "";
  const std::string key = hex_of(raw, sizeof raw);
  const mode_t old_mask = ::umask(077);
  std::ofstream out(path, std::ios::trunc);
  out << key << "\n";
  out.close();
  ::umask(old_mask);
  return out.fail() ? std::string() : key;
}

}  // namespace aios::userland
