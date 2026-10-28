// OAuth 2.0 for an installed application. See include/aios/userland/oauth.hpp.
#include "aios/userland/oauth.hpp"

#include <algorithm>
#include <cstring>

#if !defined(AIOS_METAL) && !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "aios/channel/http_client.hpp"
#include "aios/core/base64.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/net/mime.hpp"
#include "aios/node/secure.hpp"

namespace aios::userland {

namespace {

const char kVaultMagic[] = "AIOS_OAUTH_V1";

bool unreserved(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
}

int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string join_scopes(const std::vector<std::string>& s) {
  std::string o;
  for (const auto& x : s) { if (!o.empty()) o += ' '; o += x; }
  return o;
}

std::string ensure_slash_dir(std::string d) {
  while (d.size() > 1 && d.back() == '/') d.pop_back();
  return d;
}

}  // namespace

std::string oauth_url_encode(std::string_view s) {
  static const char* hex = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (unreserved(c)) o += static_cast<char>(c);
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

std::string oauth_url_decode(std::string_view s, bool plus_is_space) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() + 0 && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) {
      o += static_cast<char>(hexv(s[i + 1]) * 16 + hexv(s[i + 2]));
      i += 2;
    } else if (s[i] == '+' && plus_is_space) {
      o += ' ';
    } else {
      o += s[i];
    }
  }
  return o;
}

std::vector<std::pair<std::string, std::string>> oauth_parse_query(std::string_view q) {
  // a whole URL, a request target ("/?a=b"), or the bare query
  size_t qm = q.find('?');
  if (qm != std::string_view::npos) q = q.substr(qm + 1);
  size_t hash = q.find('#');
  if (hash != std::string_view::npos) q = q.substr(0, hash);
  size_t sp = q.find(' ');
  if (sp != std::string_view::npos) q = q.substr(0, sp);
  std::vector<std::pair<std::string, std::string>> out;
  size_t pos = 0;
  while (pos <= q.size()) {
    size_t amp = q.find('&', pos);
    std::string_view part = q.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
    if (!part.empty()) {
      size_t eq = part.find('=');
      if (eq == std::string_view::npos) out.emplace_back(oauth_url_decode(part), std::string());
      else out.emplace_back(oauth_url_decode(part.substr(0, eq)), oauth_url_decode(part.substr(eq + 1)));
    }
    if (amp == std::string_view::npos) break;
    pos = amp + 1;
  }
  return out;
}

std::string oauth_base64url(const uint8_t* data, size_t n) {
  std::string b = aios::base64_encode(aios::ByteSpan(data, n));
  for (auto& c : b) { if (c == '+') c = '-'; else if (c == '/') c = '_'; }
  while (!b.empty() && b.back() == '=') b.pop_back();
  return b;
}

std::string oauth_pkce_challenge(const std::string& verifier) {
  auto h = crypto::sha256(aios::ByteSpan(reinterpret_cast<const uint8_t*>(verifier.data()), verifier.size()));
  return oauth_base64url(h.data(), h.size());
}

OAuth::OAuth(hal::Host& host, std::string root, std::string trust_store)
    : host_(host), root_(ensure_slash_dir(std::move(root))), dir_(root_ + "/oauth"), trust_store_(std::move(trust_store)) {}

int64_t OAuth::now_s() const { return static_cast<int64_t>(host_.wall_ns() / 1000000000ull); }

// --- configuration ---------------------------------------------------------------------------------

Result<OAuthClientConfig> OAuth::load_config(const std::string& provider) {
  for (char c : provider)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
      return Result<OAuthClientConfig>(Status::Invalid, "a provider name is lowercase letters, digits, - and _");
  const std::string path = dir_ + "/" + provider + ".json";
  auto raw = host_.read_file(path);
  if (!raw.ok())
    return Result<OAuthClientConfig>(Status::NotFound, path + " does not exist: this machine has no OAuth client for " + provider +
                                                           " (save the client_secret.json from the provider's console there)");
  auto doc = json::parse(std::string_view(reinterpret_cast<const char*>(raw->data()), raw->size()));
  if (!doc.ok()) return Result<OAuthClientConfig>(Status::Invalid, path + ": " + doc.error().message);

  OAuthClientConfig c;
  c.provider = provider;
  const json::Value* src = &*doc;
  if ((*doc)["installed"].is_object()) src = &(*doc)["installed"];
  else if ((*doc)["web"].is_object()) src = &(*doc)["web"];
  c.client_id = (*src)["client_id"].str();
  c.client_secret = (*src)["client_secret"].str();
  c.auth_uri = (*src)["auth_uri"].str();
  c.token_uri = (*src)["token_uri"].str();
  c.userinfo_uri = (*src)["userinfo_uri"].str();
  c.revoke_uri = (*src)["revoke_uri"].str();
  for (const auto& s : (*src)["scopes"].items()) if (s.is_string()) c.scopes.push_back(s.str());
  for (const auto& kv : (*src)["auth_params"].members()) c.auth_params.emplace_back(kv.first, kv.second.str());

  if (provider == "google") {
    if (c.auth_uri.empty()) c.auth_uri = "https://accounts.google.com/o/oauth2/v2/auth";
    if (c.token_uri.empty()) c.token_uri = "https://oauth2.googleapis.com/token";
    if (c.userinfo_uri.empty()) c.userinfo_uri = "https://openidconnect.googleapis.com/v1/userinfo";
    if (c.revoke_uri.empty()) c.revoke_uri = "https://oauth2.googleapis.com/revoke";
    if (c.scopes.empty())
      c.scopes = {"https://mail.google.com/", "https://www.googleapis.com/auth/calendar",
                  "https://www.googleapis.com/auth/contacts", "openid", "email"};
    if (c.auth_params.empty()) c.auth_params = {{"access_type", "offline"}, {"prompt", "consent"}};
  }
  if (c.client_id.empty()) return Result<OAuthClientConfig>(Status::Invalid, path + " has no client_id");
  if (c.auth_uri.empty() || c.token_uri.empty()) return Result<OAuthClientConfig>(Status::Invalid, path + " has no auth_uri and token_uri");
  if (c.scopes.empty()) return Result<OAuthClientConfig>(Status::Invalid, path + " lists no scopes");
  return c;
}

// --- the flow --------------------------------------------------------------------------------------

Result<OAuthStart> OAuth::begin(const OAuthClientConfig& cfg, const std::string& redirect_uri, const std::string& login_hint) {
  uint8_t rnd[64];
  host_.random(MutableByteSpan(rnd, sizeof rnd));
  OAuthStart st;
  st.pending.provider = cfg.provider;
  st.pending.verifier = oauth_base64url(rnd, 64);   // 86 characters, inside RFC 7636's 43..128
  uint8_t sr[24];
  host_.random(MutableByteSpan(sr, sizeof sr));
  st.pending.state = oauth_base64url(sr, sizeof sr);
  st.pending.redirect_uri = redirect_uri;
  st.pending.login_hint = login_hint;
  st.pending.started_at = now_s();

  std::string url = cfg.auth_uri;
  url += (url.find('?') == std::string::npos) ? '?' : '&';
  url += "client_id=" + oauth_url_encode(cfg.client_id);
  url += "&redirect_uri=" + oauth_url_encode(redirect_uri);
  url += "&response_type=code";
  url += "&scope=" + oauth_url_encode(join_scopes(cfg.scopes));
  url += "&state=" + oauth_url_encode(st.pending.state);
  url += "&code_challenge=" + oauth_pkce_challenge(st.pending.verifier);
  url += "&code_challenge_method=S256";
  if (!login_hint.empty()) url += "&login_hint=" + oauth_url_encode(login_hint);
  for (const auto& kv : cfg.auth_params) url += "&" + oauth_url_encode(kv.first) + "=" + oauth_url_encode(kv.second);
  st.url = std::move(url);
  return st;
}

Result<OAuthRedirect> OAuth::wait_for_redirect(hal::Listener& listener, int timeout_ms) {
  const uint64_t deadline = host_.monotonic_ns() / 1000000ull + static_cast<uint64_t>(timeout_ms);
  for (;;) {
    const uint64_t now = host_.monotonic_ns() / 1000000ull;
    if (now >= deadline) return Result<OAuthRedirect>(Status::Expired, "no sign-in came back before the time ran out");
    auto conn = listener.accept(static_cast<int>(std::min<uint64_t>(deadline - now, 1000)));
    if (!conn.ok()) {
      if (conn.error().status == Status::Busy) continue;
      return Result<OAuthRedirect>(conn.error());
    }
    // Read the request head (and nothing more: a redirect is a GET).
    std::string req;
    uint8_t tmp[2048];
    for (int rounds = 0; rounds < 20 && req.find("\r\n\r\n") == std::string::npos && req.size() < 16384; ++rounds) {
      long n = (*conn)->recv_some(MutableByteSpan(tmp, sizeof tmp), 2000);
      if (n <= 0) break;
      req.append(reinterpret_cast<const char*>(tmp), static_cast<size_t>(n));
    }
    auto reply = [&](int code, const char* reason, const std::string& body) {
      std::string r = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
                      std::to_string(body.size()) + "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n" + body;
      (void)(*conn)->send_all(ByteSpan(reinterpret_cast<const uint8_t*>(r.data()), r.size()));
      (*conn)->close();
    };
    size_t eol = req.find("\r\n");
    std::string line = req.substr(0, eol == std::string::npos ? req.size() : eol);
    if (line.compare(0, 4, "GET ") != 0) { reply(405, "Method Not Allowed", "<p>Not a sign-in redirect.</p>"); continue; }
    auto q = oauth_parse_query(line.substr(4));
    OAuthRedirect red;
    bool has = false;
    for (const auto& kv : q) {
      if (kv.first == "code") { red.code = kv.second; has = true; }
      else if (kv.first == "state") red.state = kv.second;
      else if (kv.first == "error") { red.error = kv.second; has = true; }
      else if (kv.first == "error_description") red.error_description = kv.second;
    }
    if (!has) { reply(404, "Not Found", "<p>Waiting for the sign-in redirect.</p>"); continue; }
    reply(200, "OK",
          red.error.empty()
              ? "<!doctype html><meta charset=utf-8><title>Signed in</title><body style=\"font-family:sans-serif;margin:3em\">"
                "<h2>Signed in</h2><p>You can close this tab and go back to AIOS.</p></body>"
              : "<!doctype html><meta charset=utf-8><title>Sign-in failed</title><body style=\"font-family:sans-serif;margin:3em\">"
                "<h2>Sign-in was not completed</h2><p>Go back to AIOS for the reason.</p></body>");
    return red;
  }
}

Result<OAuthRedirect> OAuth::parse_redirect(const std::string& pasted) {
  auto q = oauth_parse_query(pasted);
  OAuthRedirect red;
  bool has = false;
  for (const auto& kv : q) {
    if (kv.first == "code") { red.code = kv.second; has = true; }
    else if (kv.first == "state") red.state = kv.second;
    else if (kv.first == "error") { red.error = kv.second; has = true; }
    else if (kv.first == "error_description") red.error_description = kv.second;
  }
  if (!has) return Result<OAuthRedirect>(Status::Invalid, "that text has no code= or error= in it: paste the whole address from the browser's address bar");
  return red;
}

Result<json::Value> OAuth::post_form(const std::string& url, const std::vector<std::pair<std::string, std::string>>& form, int* http_status) {
  auto ep = channel::parse_endpoint(url);
  if (!ep.ok()) return Result<json::Value>(ep.error());
  std::string body;
  for (const auto& kv : form) {
    if (!body.empty()) body += '&';
    body += oauth_url_encode(kv.first) + "=" + oauth_url_encode(kv.second);
  }
  channel::HttpRequest req;
  req.method = "POST";
  req.endpoint = *ep;
  req.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
  req.headers.push_back({"Accept", "application/json"});
  req.body = ByteBuffer(body.begin(), body.end());
  req.timeout_ms = 30000;
  req.host = &host_;
  req.kernel_root = root_;
  req.trust_store = trust_store_;
  auto r = channel::http_send(req);
  if (!r.ok()) return Result<json::Value>(r.error());
  if (http_status) *http_status = r->status;
  std::string text = r->text();
  if (text.empty()) return json::Value::object();
  auto doc = json::parse(text);
  if (!doc.ok()) return Result<json::Value>(Status::Corrupt, "the token endpoint answered " + std::to_string(r->status) + " with something that is not JSON");
  return doc;
}

Result<OAuthToken> OAuth::token_from_response(const OAuthClientConfig& cfg, const json::Value& r, const std::string& old_refresh) {
  OAuthToken t;
  t.provider = cfg.provider;
  t.access_token = r["access_token"].str();
  t.refresh_token = r["refresh_token"].is_string() ? r["refresh_token"].str() : old_refresh;
  t.scope = r["scope"].str();
  t.token_type = r["token_type"].str_or("Bearer");
  int64_t ttl = r["expires_in"].i64(3600);
  if (ttl < 0) ttl = 0;
  t.expires_at = now_s() + ttl;
  if (t.access_token.empty()) return Result<OAuthToken>(Status::Corrupt, "the token endpoint's answer has no access_token");
  // who signed in: the id_token's email claim, which comes straight from the token endpoint over TLS
  const std::string& idt = r["id_token"].str();
  if (!idt.empty()) {
    size_t d1 = idt.find('.');
    size_t d2 = d1 == std::string::npos ? d1 : idt.find('.', d1 + 1);
    if (d1 != std::string::npos && d2 != std::string::npos) {
      std::string payload = net::mime::decode_base64(std::string_view(idt).substr(d1 + 1, d2 - d1 - 1));
      auto claims = json::parse(payload);
      if (claims.ok()) t.account = (*claims)["email"].str();
    }
  }
  return t;
}

Result<OAuthToken> OAuth::finish(const OAuthClientConfig& cfg, const OAuthPending& pending, const OAuthRedirect& red) {
  if (!red.error.empty())
    return Result<OAuthToken>(Status::Denied, "the sign-in was not completed: " + red.error + (red.error_description.empty() ? "" : " (" + red.error_description + ")"));
  // The state ties this redirect to the sign-in we started: without it anybody who can make the browser
  // visit the loopback address could hand us their own code.
  if (red.state.empty() || red.state != pending.state)
    return Result<OAuthToken>(Status::Denied, "the redirect's state does not match the sign-in that was started: refusing it");
  std::vector<std::pair<std::string, std::string>> form = {
      {"grant_type", "authorization_code"}, {"code", red.code},          {"redirect_uri", pending.redirect_uri},
      {"client_id", cfg.client_id},         {"code_verifier", pending.verifier}};
  if (!cfg.client_secret.empty()) form.push_back({"client_secret", cfg.client_secret});
  int status = 0;
  auto r = post_form(cfg.token_uri, form, &status);
  if (!r.ok()) return Result<OAuthToken>(r.error());
  if (status != 200 || (*r)["error"].is_string())
    return Result<OAuthToken>(Status::Denied, "the token endpoint refused the code: " + (*r)["error"].str() + " " + (*r)["error_description"].str());
  auto t = token_from_response(cfg, *r, "");
  if (!t.ok()) return t;
  if (t->refresh_token.empty())
    return Result<OAuthToken>(Status::Denied, "the provider issued no refresh token, so the sign-in would last an hour: ask for offline access (Google: access_type=offline, prompt=consent)");
  if (t->account.empty() && !cfg.userinfo_uri.empty()) {
    auto ep = channel::parse_endpoint(cfg.userinfo_uri);
    if (ep.ok()) {
      channel::HttpRequest req;
      req.endpoint = *ep;
      req.headers.push_back({"Authorization", "Bearer " + t->access_token});
      req.headers.push_back({"Accept", "application/json"});
      req.timeout_ms = 30000;
      req.host = &host_;
      req.kernel_root = root_;
      req.trust_store = trust_store_;
      auto ur = channel::http_send(req);
      if (ur.ok() && ur->status == 200) {
        auto doc = json::parse(ur->text());
        if (doc.ok()) t->account = (*doc)["email"].str();
      }
    }
  }
  if (t->account.empty()) t->account = pending.login_hint;
  if (t->account.empty())
    return Result<OAuthToken>(Status::Invalid, "signed in, but the provider did not say which account: pass the address (login_hint) so the tokens can be filed under it");
  return t;
}

Result<OAuthToken> OAuth::refresh(const OAuthClientConfig& cfg, const OAuthToken& token) {
  if (token.refresh_token.empty()) return Result<OAuthToken>(Status::Expired, "this sign-in has no refresh token: sign in again");
  std::vector<std::pair<std::string, std::string>> form = {
      {"grant_type", "refresh_token"}, {"refresh_token", token.refresh_token}, {"client_id", cfg.client_id}};
  if (!cfg.client_secret.empty()) form.push_back({"client_secret", cfg.client_secret});
  int status = 0;
  auto r = post_form(cfg.token_uri, form, &status);
  if (!r.ok()) return Result<OAuthToken>(r.error());
  if (status != 200 || (*r)["error"].is_string()) {
    const std::string err = (*r)["error"].str();
    if (err == "invalid_grant")
      return Result<OAuthToken>(Status::Expired, "the provider no longer honours this sign-in (revoked, or unused too long): sign in again");
    return Result<OAuthToken>(Status::IoError, "refreshing the token failed: " + err + " " + (*r)["error_description"].str());
  }
  auto t = token_from_response(cfg, *r, token.refresh_token);
  if (!t.ok()) return t;
  t->account = token.account;
  return t;
}

Result<std::string> OAuth::access_token(const std::string& provider, const std::string& account, int skew_seconds) {
  auto t = load_token(provider, account);
  if (!t.ok()) return Result<std::string>(t.error());
  if (!t->access_token.empty() && t->expires_at - now_s() > skew_seconds) return t->access_token;
  auto cfg = load_config(provider);
  if (!cfg.ok()) return Result<std::string>(cfg.error());
  auto nt = refresh(*cfg, *t);
  if (!nt.ok()) return Result<std::string>(nt.error());
  if (auto s = save_token(*nt); !s.ok()) return Result<std::string>(s.error());
  return nt->access_token;
}

// --- the vault -------------------------------------------------------------------------------------
//
// tokens.vault holds every token and any sign-in in progress, sealed with ChaCha20-Poly1305 under a random
// 32-byte key kept in vault.key beside it (mode 0600 where the host has modes). That keeps the tokens out of
// backups, logs, a stray `cat` and a repository; it does not protect them from somebody who can read this
// directory as the same user, because the key has to be somewhere this program can reach without a person
// typing a passphrase at every boot. A refresh token is revocable at the provider (OAuth::revoke).

Result<json::Value> OAuth::read_vault() {
  const std::string vpath = dir_ + "/tokens.vault", kpath = dir_ + "/vault.key";
  if (!host_.exists(vpath)) return json::Value::object();
  auto key_raw = host_.read_file(kpath);
  if (!key_raw.ok() || key_raw->size() != 32)
    return Result<json::Value>(Status::Corrupt, kpath + " is missing or damaged, so " + vpath + " cannot be opened; sign in again after removing both");
  auto blob = host_.read_file(vpath);
  if (!blob.ok()) return Result<json::Value>(blob.error());
  const size_t magic = sizeof kVaultMagic;   // includes the NUL; written as "AIOS_OAUTH_V1\0"
  if (blob->size() < magic + 12 + 4 + 16 || std::memcmp(blob->data(), kVaultMagic, magic) != 0)
    return Result<json::Value>(Status::Corrupt, vpath + " is not a token vault");
  crypto::Key key;
  std::memcpy(key.data(), key_raw->data(), 32);
  crypto::Nonce nonce;
  std::memcpy(nonce.data(), blob->data() + magic, 12);
  uint32_t len = 0;
  std::memcpy(&len, blob->data() + magic + 12, 4);
  if (magic + 16 + len != blob->size()) return Result<json::Value>(Status::Corrupt, vpath + " is truncated");
  auto plain = crypto::aead_decrypt(key, nonce, ByteSpan(reinterpret_cast<const uint8_t*>(kVaultMagic), magic - 1),
                                    ByteSpan(blob->data() + magic + 16, len));
  if (!plain.ok()) return Result<json::Value>(Status::Corrupt, vpath + " fails its integrity check (wrong key, or altered)");
  auto doc = json::parse(std::string_view(reinterpret_cast<const char*>(plain->data()), plain->size()));
  if (!doc.ok()) return Result<json::Value>(Status::Corrupt, vpath + " decrypts to something that is not JSON");
  return doc;
}

Result<void> OAuth::write_vault(const json::Value& v) {
  const std::string vpath = dir_ + "/tokens.vault", kpath = dir_ + "/vault.key";
  if (!host_.exists(dir_)) {
    auto m = host_.make_dir(dir_);
    if (!m.ok() && !host_.exists(dir_)) return m;
#if !defined(AIOS_METAL) && !defined(_WIN32)
    ::chmod(dir_.c_str(), 0700);
#endif
  }
  crypto::Key key;
  auto key_raw = host_.exists(kpath) ? host_.read_file(kpath) : Result<ByteBuffer>(Status::NotFound);
  if (key_raw.ok() && key_raw->size() == 32) {
    std::memcpy(key.data(), key_raw->data(), 32);
  } else {
    if (host_.exists(vpath))
      return Result<void>(Status::Corrupt, kpath + " is missing or damaged while " + vpath + " exists: refusing to overwrite the vault");
    host_.random(MutableByteSpan(key.data(), key.size()));
    if (auto w = host_.write_file(kpath, ByteSpan(key.data(), key.size())); !w.ok()) return w;
#if !defined(AIOS_METAL) && !defined(_WIN32)
    ::chmod(kpath.c_str(), 0600);
#endif
  }
  crypto::Nonce nonce;
  host_.random(MutableByteSpan(nonce.data(), nonce.size()));   // fresh for every write
  const std::string plain = v.dump();
  const size_t magic = sizeof kVaultMagic;
  ByteBuffer ct = crypto::aead_encrypt(key, nonce, ByteSpan(reinterpret_cast<const uint8_t*>(kVaultMagic), magic - 1),
                                       ByteSpan(reinterpret_cast<const uint8_t*>(plain.data()), plain.size()));
  // magic, nonce, the length (4 bytes, little-endian), the ciphertext: written into place at their final size
  ByteBuffer out(magic + nonce.size() + 4 + ct.size());
  std::memcpy(out.data(), kVaultMagic, magic);
  std::memcpy(out.data() + magic, nonce.data(), nonce.size());
  const uint32_t len = static_cast<uint32_t>(ct.size());
  for (int i = 0; i < 4; ++i) out[magic + nonce.size() + (size_t)i] = (uint8_t)(len >> (8 * i));
  std::memcpy(out.data() + magic + nonce.size() + 4, ct.data(), ct.size());
  const std::string tmp = vpath + ".tmp";
  if (auto w = host_.write_file(tmp, ByteSpan(out.data(), out.size())); !w.ok()) return w;
#if !defined(AIOS_METAL) && !defined(_WIN32)
  ::chmod(tmp.c_str(), 0600);
#endif
  return host_.rename_file(tmp, vpath);
}

static json::Value token_json(const OAuthToken& t) {
  json::Value o = json::Value::object();
  o.set("provider", json::Value::string(t.provider));
  o.set("account", json::Value::string(t.account));
  o.set("access_token", json::Value::string(t.access_token));
  o.set("refresh_token", json::Value::string(t.refresh_token));
  o.set("expires_at", json::Value::integer(t.expires_at));
  o.set("scope", json::Value::string(t.scope));
  o.set("token_type", json::Value::string(t.token_type));
  return o;
}

static OAuthToken token_from_json(const json::Value& o) {
  OAuthToken t;
  t.provider = o["provider"].str();
  t.account = o["account"].str();
  t.access_token = o["access_token"].str();
  t.refresh_token = o["refresh_token"].str();
  t.expires_at = o["expires_at"].i64();
  t.scope = o["scope"].str();
  t.token_type = o["token_type"].str();
  return t;
}

Result<void> OAuth::save_token(const OAuthToken& t) {
  auto v = read_vault();
  if (!v.ok()) return Result<void>(v.error());
  json::Value doc = *v;
  json::Value list = json::Value::array();
  bool replaced = false;
  for (const auto& e : doc["tokens"].items()) {
    if (e["provider"].str() == t.provider && e["account"].str() == t.account) { list.push(token_json(t)); replaced = true; }
    else list.push(e);
  }
  if (!replaced) list.push(token_json(t));
  doc.set("tokens", std::move(list));
  return write_vault(doc);
}

Result<OAuthToken> OAuth::load_token(const std::string& provider, const std::string& account) {
  auto v = read_vault();
  if (!v.ok()) return Result<OAuthToken>(v.error());
  for (const auto& e : (*v)["tokens"].items())
    if (e["provider"].str() == provider && e["account"].str() == account) return token_from_json(e);
  return Result<OAuthToken>(Status::NotFound, "no " + provider + " sign-in for " + account);
}

Result<std::vector<OAuthToken>> OAuth::list_tokens() {
  auto v = read_vault();
  if (!v.ok()) return Result<std::vector<OAuthToken>>(v.error());
  std::vector<OAuthToken> out;
  for (const auto& e : (*v)["tokens"].items()) out.push_back(token_from_json(e));
  return out;
}

Result<void> OAuth::forget_token(const std::string& provider, const std::string& account) {
  auto v = read_vault();
  if (!v.ok()) return Result<void>(v.error());
  json::Value doc = *v;
  json::Value list = json::Value::array();
  for (const auto& e : doc["tokens"].items())
    if (!(e["provider"].str() == provider && e["account"].str() == account)) list.push(e);
  doc.set("tokens", std::move(list));
  return write_vault(doc);
}

Result<void> OAuth::revoke(const std::string& provider, const std::string& account) {
  auto t = load_token(provider, account);
  if (!t.ok()) return Result<void>(t.error());
  auto cfg = load_config(provider);
  std::string warning;
  if (cfg.ok() && !cfg->revoke_uri.empty()) {
    int status = 0;
    auto r = post_form(cfg->revoke_uri, {{"token", t->refresh_token.empty() ? t->access_token : t->refresh_token}}, &status);
    if (!r.ok() || (status != 200 && status != 400))   // 400 is "already invalid": as good as revoked
      warning = r.ok() ? "the provider answered " + std::to_string(status) : r.error().message;
  }
  if (auto f = forget_token(provider, account); !f.ok()) return f;
  if (!warning.empty()) return Result<void>(Status::IoError, "forgotten here, but the provider could not be told to revoke it (" + warning + "): revoke it in the account's security settings");
  return Ok();
}

Result<void> OAuth::save_pending(const OAuthPending& p) {
  auto v = read_vault();
  if (!v.ok()) return Result<void>(v.error());
  json::Value doc = *v;
  json::Value pend = doc["pending"].is_object() ? doc["pending"] : json::Value::object();
  json::Value o = json::Value::object();
  o.set("state", json::Value::string(p.state));
  o.set("verifier", json::Value::string(p.verifier));
  o.set("redirect_uri", json::Value::string(p.redirect_uri));
  o.set("login_hint", json::Value::string(p.login_hint));
  o.set("started_at", json::Value::integer(p.started_at));
  pend.set(p.provider, std::move(o));
  doc.set("pending", std::move(pend));
  return write_vault(doc);
}

Result<OAuthPending> OAuth::take_pending(const std::string& provider) {
  auto v = read_vault();
  if (!v.ok()) return Result<OAuthPending>(v.error());
  const json::Value& o = (*v)["pending"][provider];
  if (!o.is_object()) return Result<OAuthPending>(Status::NotFound, "no sign-in is in progress for " + provider + ": start one first");
  OAuthPending p;
  p.provider = provider;
  p.state = o["state"].str();
  p.verifier = o["verifier"].str();
  p.redirect_uri = o["redirect_uri"].str();
  p.login_hint = o["login_hint"].str();
  p.started_at = o["started_at"].i64();
  if (now_s() - p.started_at > 3600) {
    json::Value doc = *v;
    json::Value pend = doc["pending"];
    pend.erase(provider);
    doc.set("pending", std::move(pend));
    (void)write_vault(doc);
    return Result<OAuthPending>(Status::Expired, "that sign-in was started over an hour ago: start a new one");
  }
  json::Value doc = *v;
  json::Value pend = doc["pending"];
  pend.erase(provider);
  doc.set("pending", std::move(pend));
  if (auto w = write_vault(doc); !w.ok()) return Result<OAuthPending>(w.error());
  return p;
}

}  // namespace aios::userland
