#pragma once
// OAuth 2.0 for an installed application (RFC 6749, 7636, 8252): authorization code with PKCE S256 and a
// loopback redirect, the token endpoint, refresh, and an encrypted store that keeps the tokens across
// reboots. Provider-neutral (Google, Microsoft, anything with the same endpoints): the endpoints, client id
// and scopes come from <root>/oauth/<provider>.json, which for Google is the client_secret.json that Google
// Cloud Console hands out, unchanged. Nothing is shelled out to; it builds for the bare-metal image.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "aios/core/json.hpp"
#include "aios/core/result.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/net.hpp"

namespace aios::userland {

struct OAuthClientConfig {
  std::string provider;                 // "google"
  std::string client_id;
  std::string client_secret;            // an installed app's "secret" is not secret, but Google's token endpoint wants it
  std::string auth_uri;
  std::string token_uri;
  std::string userinfo_uri;             // optional: where to ask who signed in when the token carries no id_token
  std::string revoke_uri;               // optional: where a token is revoked
  std::vector<std::string> scopes;
  std::vector<std::pair<std::string, std::string>> auth_params;   // extra query parameters on the consent URL
};

struct OAuthToken {
  std::string provider;
  std::string account;                  // the email the token belongs to
  std::string access_token;
  std::string refresh_token;
  int64_t expires_at = 0;               // epoch seconds
  std::string scope;
  std::string token_type;
};

struct OAuthPending {                   // a sign-in that has been started and not finished
  std::string provider, state, verifier, redirect_uri, login_hint;
  int64_t started_at = 0;
};

struct OAuthStart {
  std::string url;                      // open this in a browser
  OAuthPending pending;
};

struct OAuthRedirect {                  // what came back on the redirect
  std::string code, state, error, error_description;
};

// --- helpers, exposed for tests ---------------------------------------------------------------------
std::string oauth_url_encode(std::string_view s);                              // RFC 3986 unreserved kept, rest %XX
std::string oauth_url_decode(std::string_view s, bool plus_is_space = true);
std::vector<std::pair<std::string, std::string>> oauth_parse_query(std::string_view q);   // "a=1&b=%20" or a whole URL
std::string oauth_base64url(const uint8_t* data, size_t n);                    // no padding
// PKCE: the S256 challenge of a verifier.
std::string oauth_pkce_challenge(const std::string& verifier);

class OAuth {
 public:
  // `root` is the machine's root; state lives in <root>/oauth. `trust_store` overrides the TLS trust store.
  OAuth(hal::Host& host, std::string root, std::string trust_store = {});

  // <root>/oauth/<provider>.json: Google's client_secret.json ({"installed": {...}} or {"web": {...}}) or
  // {"client_id","client_secret","auth_uri","token_uri","scopes":[...],"auth_params":{...}}. For "google",
  // scopes and endpoints default to what mail, calendar and contacts need.
  Result<OAuthClientConfig> load_config(const std::string& provider);

  // Step one: a consent URL and the secrets that must survive until the redirect comes back. The caller
  // keeps `pending` (save_pending) for a sign-in finished in another process or after a reboot.
  Result<OAuthStart> begin(const OAuthClientConfig& cfg, const std::string& redirect_uri, const std::string& login_hint = {});

  // Wait on a loopback listener for the browser's redirect. Answers the browser with a short page.
  // Requests that are not the redirect (a favicon, a probe) are answered and ignored.
  Result<OAuthRedirect> wait_for_redirect(hal::Listener& listener, int timeout_ms);
  // The same from a pasted URL (or just its query): for a browser on another machine.
  static Result<OAuthRedirect> parse_redirect(const std::string& pasted);

  // Step two: the code for tokens. Checks the state, asks the token endpoint, learns who signed in.
  Result<OAuthToken> finish(const OAuthClientConfig& cfg, const OAuthPending& pending, const OAuthRedirect& redirect);

  Result<OAuthToken> refresh(const OAuthClientConfig& cfg, const OAuthToken& token);
  // A usable access token for (provider, account): refreshed and re-saved if it is within `skew` seconds of
  // expiring. Expired when the refresh token is gone or revoked: the person has to sign in again.
  Result<std::string> access_token(const std::string& provider, const std::string& account, int skew_seconds = 120);

  // The store (encrypted at rest; see oauth.cpp for what that does and does not protect against).
  Result<void> save_token(const OAuthToken& t);
  Result<OAuthToken> load_token(const std::string& provider, const std::string& account);
  Result<std::vector<OAuthToken>> list_tokens();
  Result<void> forget_token(const std::string& provider, const std::string& account);   // locally; see revoke()
  Result<void> revoke(const std::string& provider, const std::string& account);          // asks the provider, then forgets
  Result<void> save_pending(const OAuthPending& p);
  Result<OAuthPending> take_pending(const std::string& provider);   // and removes it

  const std::string& dir() const { return dir_; }

 private:
  Result<json::Value> read_vault();
  Result<void> write_vault(const json::Value& v);
  Result<json::Value> post_form(const std::string& url, const std::vector<std::pair<std::string, std::string>>& form, int* http_status);
  Result<OAuthToken> token_from_response(const OAuthClientConfig& cfg, const json::Value& r, const std::string& old_refresh);
  int64_t now_s() const;

  hal::Host& host_;
  std::string root_, dir_, trust_store_;
};

}  // namespace aios::userland
