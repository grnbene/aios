#include "aios/userland/sql_secrets.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <string>

#include "aios/core/bytes.hpp"
#include "aios/crypto/aes_gcm.hpp"
#include "aios/hal/host.hpp"

namespace aios::userland {

namespace {

bool name_ok(const std::string& n) {
  if (n.empty() || n.size() > 64) return false;
  for (unsigned char c : n) if (!(std::isalnum(c) || c == '_')) return false;
  return true;
}

std::string hex_of(const unsigned char* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string o;
  o.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) { o += d[p[i] >> 4]; o += d[p[i] & 15]; }
  return o;
}

int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool unhex(const std::string& s, std::vector<unsigned char>* out) {
  if (s.size() % 2) return false;
  out->clear();
  for (size_t i = 0; i < s.size(); i += 2) {
    const int a = hexv(s[i]), b = hexv(s[i + 1]);
    if (a < 0 || b < 0) return false;
    out->push_back((unsigned char)(a * 16 + b));
  }
  return true;
}

std::string now_text() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char b[32];
  std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
  return b;
}

}  // namespace

// The key: AIOS_SECRET_KEY_FILE when set (it must exist), else <dir>/master.key, made on first use.
bool SecretStore::key_locked(unsigned char out[32], std::string* err) {
  const char* env = std::getenv("AIOS_SECRET_KEY_FILE");
  const std::string path = (env && *env) ? std::string(env) : dir_ + "/master.key";
  auto take = [&](const aios::ByteBuffer& raw) -> bool {
    if (raw.size() == 32) { std::copy(raw.begin(), raw.end(), out); return true; }
    std::string t = string_of(ByteSpan(raw.data(), raw.size()));
    while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
    std::vector<unsigned char> b;
    if (t.size() == 64 && unhex(t, &b)) { std::copy(b.begin(), b.end(), out); return true; }
    return false;
  };
  if (host_.exists(path)) {
    auto r = host_.read_file(path);
    if (!r.ok()) { *err = "secret key " + path + ": " + r.error().message; return false; }
    if (!take(*r)) { *err = "secret key " + path + " is neither 32 raw bytes nor 64 hex characters"; return false; }
    return true;
  }
  if (env && *env) { *err = "AIOS_SECRET_KEY_FILE " + path + " does not exist"; return false; }
  host_.make_dir(dir_);
  aios::ByteBuffer fresh(32);
  host_.random(aios::MutableByteSpan(fresh.data(), fresh.size()));
  auto w = host_.create_exclusive_private(path, aios::ByteSpan(fresh.data(), fresh.size()));
  if (!w.ok()) {
    // another thread or process made it first: use theirs
    auto r = host_.read_file(path);
    if (r.ok() && take(*r)) return true;
    *err = "cannot create the secret key " + path + ": " + w.error().message;
    return false;
  }
  std::copy(fresh.begin(), fresh.end(), out);
  return true;
}

bool SecretStore::load_locked(std::string* err) {
  if (loaded_) return true;
  items_.clear();
  const std::string path = dir_ + "/secrets.db";
  if (host_.exists(path)) {
    auto r = host_.read_file(path);
    if (!r.ok()) { *err = "secrets.db: " + r.error().message; return false; }
    const std::string text = string_of(ByteSpan(r->data(), r->size()));
    size_t at = 0;
    while (at < text.size()) {
      size_t nl = text.find('\n', at);
      const std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
      at = nl == std::string::npos ? text.size() : nl + 1;
      const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
      if (b == std::string::npos) continue;
      Item it;
      it.name = line.substr(0, a);
      it.at = line.substr(a + 1, b - a - 1);
      if (!name_ok(it.name) || !unhex(line.substr(b + 1), &it.sealed) || it.sealed.size() < 12 + 16) {
        *err = "secrets.db is damaged at '" + it.name + "'";
        return false;
      }
      items_.push_back(std::move(it));
    }
  }
  loaded_ = true;
  return true;
}

bool SecretStore::save_locked(std::string* err) {
  host_.make_dir(dir_);
  std::string text;
  for (const Item& it : items_) text += it.name + "\t" + it.at + "\t" + hex_of(it.sealed.data(), it.sealed.size()) + "\n";
  const std::string path = dir_ + "/secrets.db";
  auto w = host_.write_file(path, aios::span_of(text));
  if (!w.ok()) { *err = "cannot write secrets.db: " + w.error().message; return false; }
  host_.set_access(path, aios::hal::Host::FileAccess::Private);
  return true;
}

bool SecretStore::set(const std::string& name, const std::string& value, std::string* err) {
  if (!name_ok(name)) { *err = "a secret's name is letters, digits and _ (up to 64)"; return false; }
  if (value.empty()) { *err = "a secret needs a value"; return false; }
  std::lock_guard<std::mutex> lk(mu_);
  if (!load_locked(err)) return false;
  unsigned char k[32];
  if (!key_locked(k, err)) return false;
  aios::crypto::Aes256Key key;
  std::copy(k, k + 32, key.begin());
  aios::crypto::GcmNonce nonce;
  host_.random(aios::MutableByteSpan(nonce.data(), nonce.size()));
  const std::string aad = name;
  aios::ByteBuffer ct = aios::crypto::aes256_gcm_seal(key, nonce, aios::span_of(aad), aios::span_of(value));
  Item it;
  it.name = name;
  it.at = now_text();
  it.sealed.assign(nonce.begin(), nonce.end());
  it.sealed.insert(it.sealed.end(), ct.begin(), ct.end());
  std::fill(k, k + 32, 0);
  bool replaced = false;
  for (Item& o : items_) if (o.name == name) { o = it; replaced = true; }
  if (!replaced) items_.push_back(std::move(it));
  return save_locked(err);
}

bool SecretStore::drop(const std::string& name, std::string* err) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!load_locked(err)) return false;
  auto it = std::find_if(items_.begin(), items_.end(), [&](const Item& i) { return i.name == name; });
  if (it == items_.end()) { *err = "no secret named " + name; return false; }
  items_.erase(it);
  return save_locked(err);
}

bool SecretStore::has(const std::string& name) {
  std::lock_guard<std::mutex> lk(mu_);
  std::string err;
  if (!load_locked(&err)) return false;
  for (const Item& i : items_) if (i.name == name) return true;
  return false;
}

bool SecretStore::get(const std::string& name, std::string* value, std::string* err) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!load_locked(err)) return false;
  const Item* found = nullptr;
  for (const Item& i : items_) if (i.name == name) found = &i;
  if (!found) { *err = "no secret named " + name; return false; }
  unsigned char k[32];
  if (!key_locked(k, err)) return false;
  aios::crypto::Aes256Key key;
  std::copy(k, k + 32, key.begin());
  std::fill(k, k + 32, 0);
  aios::crypto::GcmNonce nonce;
  std::copy(found->sealed.begin(), found->sealed.begin() + 12, nonce.begin());
  const std::string aad = name;
  aios::ByteBuffer plain;
  if (!aios::crypto::aes256_gcm_open(key, nonce, aios::span_of(aad),
                                     aios::ByteSpan(found->sealed.data() + 12, found->sealed.size() - 12), plain)) {
    *err = "secret " + name + " does not open: the key is not the one it was sealed with, or the file was altered";
    return false;
  }
  value->assign(reinterpret_cast<const char*>(plain.data()), plain.size());
  return true;
}

std::vector<SecretStore::Entry> SecretStore::list() {
  std::lock_guard<std::mutex> lk(mu_);
  std::string err;
  std::vector<Entry> out;
  if (!load_locked(&err)) return out;
  for (const Item& i : items_) out.push_back({i.name, i.at});
  std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
  return out;
}

std::string SecretStore::scrub(std::string text) {
  std::vector<std::string> names;
  {
    std::lock_guard<std::mutex> lk(mu_);
    std::string err;
    if (!load_locked(&err)) return text;
    for (const Item& i : items_) names.push_back(i.name);
  }
  for (const std::string& n : names) {
    std::string v, err;
    if (!get(n, &v, &err) || v.empty()) continue;
    for (size_t p = text.find(v); p != std::string::npos; p = text.find(v, p + 3)) text.replace(p, v.size(), "***");
  }
  return text;
}

std::string SecretStore::redact(const std::string& sql) {
  size_t i = 0;
  while (i < sql.size() && std::isspace((unsigned char)sql[i])) ++i;
  auto word = [&](const char* w) -> bool {
    size_t j = i;
    for (const char* c = w; *c; ++c, ++j)
      if (j >= sql.size() || std::toupper((unsigned char)sql[j]) != *c) return false;
    if (j < sql.size() && !std::isspace((unsigned char)sql[j])) return false;
    i = j;
    while (i < sql.size() && std::isspace((unsigned char)sql[i])) ++i;
    return true;
  };
  if (!word("CREATE") || !word("SECRET")) return sql;
  size_t b = i;
  while (i < sql.size() && !std::isspace((unsigned char)sql[i])) ++i;
  const std::string name = sql.substr(b, i - b);
  return "CREATE SECRET " + (name.empty() ? std::string("?") : name) + " AS '***'";
}

}  // namespace aios::userland
