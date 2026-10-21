// Drives the in-tree OAuth 2.0 client against scripts/mock-google.py: the whole installed-app flow with a
// simulated browser, tokens kept across a simulated reboot, refresh, revocation, and every way the flow
// is supposed to refuse. The mock enforces PKCE, single-use codes, the client secret and token expiry.
//   aios-oauth-check <mock-port> <scratch-dir>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#include "aios/channel/http_client.hpp"
#include "aios/hal/host.hpp"
#include "aios/userland/oauth.hpp"

using namespace aios;
using namespace aios::userland;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++g_fail;
}
static std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static hal::Host* g_host = nullptr;
static std::string http_get(const std::string& url, std::string* location = nullptr, int* status = nullptr) {
  auto ep = channel::parse_endpoint(url);
  channel::HttpRequest req;
  req.endpoint = *ep;
  req.host = g_host;
  req.timeout_ms = 10000;
  auto r = channel::http_send(req);
  if (!r.ok()) return "ERR " + r.error().message;
  if (status) *status = r->status;
  if (location) *location = r->header("Location");
  return r->text();
}
static std::string http_post(const std::string& url) {
  auto ep = channel::parse_endpoint(url);
  channel::HttpRequest req;
  req.method = "POST";
  req.endpoint = *ep;
  req.host = g_host;
  req.timeout_ms = 10000;
  auto r = channel::http_send(req);
  return r.ok() ? r->text() : "ERR";
}
static int counter(const std::string& base, const char* name) {
  auto doc = json::parse(http_get(base + "/_state"));
  return doc.ok() ? static_cast<int>((*doc)[name].i64()) : -1;
}

// The person: opens the consent URL in a browser, which is sent on to the redirect URI.
static void browser(const std::string& auth_url) {
  std::string loc;
  int st = 0;
  http_get(auth_url, &loc, &st);
  if (st == 302 && !loc.empty()) http_get(loc);
}

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: %s <mock-port> <scratch-dir>\n", argv[0]); return 2; }
  const std::string base = std::string("http://127.0.0.1:") + argv[1];
  const std::string root = argv[2];
  auto host = hal::make_host();
  g_host = host.get();
  (void)host->make_dir(root);

  // --- helpers against the RFC ---
  check(oauth_pkce_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM",
        "PKCE S256 matches RFC 7636 appendix B's test vector");
  check(oauth_url_encode("a b&c=d/é~") == "a%20b%26c%3Dd%2F%C3%A9~", "URL encoding keeps only unreserved characters");
  check(oauth_url_decode("a%20b+c%C3%A9") == "a b cé", "URL decoding handles %XX and +");
  auto q = oauth_parse_query("http://127.0.0.1:5/?code=4%2F0AX&state=s+t&scope=a%20b#frag");
  check(q.size() == 3 && q[0].second == "4/0AX" && q[1].second == "s t" && q[2].second == "a b", "a whole redirect URL parses to its parameters");

  // --- configuration ---
  {
    OAuth o(*host, root);
    auto none = o.load_config("mock");
    check(!none.ok() && none.error().status == Status::NotFound, "no client file: NotFound, and the message says where to put it");
  }
  (void)host->make_dir(root + "/oauth");
  {
    std::string cfg = "{\"installed\":{\"client_id\":\"mock-client.apps.googleusercontent.com\",\"client_secret\":\"mock-secret\","
                      "\"auth_uri\":\"" + base + "/auth\",\"token_uri\":\"" + base + "/token\",\"userinfo_uri\":\"" + base + "/userinfo\","
                      "\"revoke_uri\":\"" + base + "/revoke\",\"scopes\":[\"https://mail.google.com/\",\"openid\",\"email\"],"
                      "\"auth_params\":{\"access_type\":\"offline\",\"prompt\":\"consent\"}}}";
    (void)host->write_file(root + "/oauth/mock.json", ByteSpan(reinterpret_cast<const uint8_t*>(cfg.data()), cfg.size()));
  }
  OAuth oauth(*host, root);
  auto cfg = oauth.load_config("mock");
  check(cfg.ok() && cfg->client_id == "mock-client.apps.googleusercontent.com" && cfg->scopes.size() == 3, "load_config reads Google's client_secret.json shape");
  if (!cfg.ok()) { std::printf("cannot continue: %s\n", cfg.error().message.c_str()); return 1; }

  // --- the flow, listener and all ---
  auto lis = hal::net_listen(0, true);
  check(lis.ok() && (*lis)->port() != 0, "a loopback listener on a free port");
  const std::string redirect = "http://127.0.0.1:" + std::to_string((*lis)->port()) + "/";
  auto st = oauth.begin(*cfg, redirect, "sean@example.com");
  check(st.ok() && st->url.find("code_challenge_method=S256") != std::string::npos && st->url.find("access_type=offline") != std::string::npos &&
            st->url.find("state=") != std::string::npos && st->pending.verifier.size() >= 43 && st->pending.verifier.size() <= 128,
        "begin(): consent URL carries PKCE S256, state, offline access; the verifier is within RFC 7636's length");
  auto st2 = oauth.begin(*cfg, redirect);
  check(st2.ok() && st2->pending.state != st->pending.state && st2->pending.verifier != st->pending.verifier, "every sign-in gets a fresh state and verifier");

  std::thread t([&] { browser(st->url); });
  auto red = oauth.wait_for_redirect(**lis, 15000);
  t.join();
  check(red.ok() && !red->code.empty() && red->state == st->pending.state, "the redirect arrives on the loopback listener with the code and our state");
  auto tok = oauth.finish(*cfg, st->pending, *red);
  check(tok.ok() && tok->account == "sean@example.com" && !tok->access_token.empty() && !tok->refresh_token.empty(),
        "finish(): access and refresh tokens, and the account read from the id_token");
  if (!tok.ok()) { std::printf("cannot continue: %s\n", tok.error().message.c_str()); return 1; }
  check(counter(base, "token_auth_code") == 1, "exactly one code exchange reached the token endpoint");
  check(oauth.save_token(*tok).ok(), "the token is saved");

  // --- stays after a reboot ---
  {
    OAuth after_reboot(*host, root);
    auto back = after_reboot.load_token("mock", "sean@example.com");
    check(back.ok() && back->access_token == tok->access_token && back->refresh_token == tok->refresh_token, "a new OAuth object (a reboot) finds the same tokens on disk");
    auto at = after_reboot.access_token("mock", "sean@example.com");
    check(at.ok() && *at == tok->access_token && counter(base, "token_refresh") == 0, "a still-valid token is used as it is: no network round trip");
  }

  // --- the vault at rest ---
  {
    std::string v = slurp(root + "/oauth/tokens.vault");
    check(!v.empty() && v.find(tok->access_token) == std::string::npos && v.find(tok->refresh_token) == std::string::npos && v.find("sean@example.com") == std::string::npos,
          "the vault file holds neither token nor the address in the clear");
    check(slurp(root + "/oauth/vault.key").size() == 32, "the key is 32 random bytes in its own file");
    OAuthToken other = *tok;
    other.account = "b@example.com";
    (void)oauth.save_token(other);
    std::string v2 = slurp(root + "/oauth/tokens.vault");
    check(v2 != v && v2.substr(14, 12) != v.substr(14, 12), "every write uses a fresh nonce");
    auto all = oauth.list_tokens();
    check(all.ok() && all->size() == 2, "two accounts under one provider are kept apart");
    check(oauth.forget_token("mock", "b@example.com").ok() && oauth.list_tokens()->size() == 1, "forgetting one leaves the other");
    // tamper with a ciphertext byte
    std::string bad = slurp(root + "/oauth/tokens.vault");
    bad[bad.size() - 20] ^= 0x01;
    (void)host->write_file(root + "/oauth/tokens.vault", ByteSpan(reinterpret_cast<const uint8_t*>(bad.data()), bad.size()));
    OAuth o2(*host, root);
    auto r = o2.load_token("mock", "sean@example.com");
    check(!r.ok() && r.error().status == Status::Corrupt, "a flipped bit fails the integrity check (Corrupt), it is not read as garbage");
    (void)host->write_file(root + "/oauth/tokens.vault", ByteSpan(reinterpret_cast<const uint8_t*>(v2.data()), v2.size()));   // put it back
    // lose the key
    std::string key = slurp(root + "/oauth/vault.key");
    (void)host->remove(root + "/oauth/vault.key");
    OAuth o3(*host, root);
    auto r3 = o3.load_token("mock", "sean@example.com");
    check(!r3.ok() && r3.error().status == Status::Corrupt, "a lost key is reported, never papered over");
    auto w3 = o3.save_token(*tok);
    check(!w3.ok(), "and a lost key does not let a save overwrite the vault with a new one");
    (void)host->write_file(root + "/oauth/vault.key", ByteSpan(reinterpret_cast<const uint8_t*>(key.data()), key.size()));
    check(oauth.forget_token("mock", "b@example.com").ok() && oauth.load_token("mock", "sean@example.com").ok(), "restored, everything reads again");
  }

  // --- refresh ---
  {
    OAuthToken old = *tok;
    old.expires_at = 1;   // long gone
    (void)oauth.save_token(old);
    OAuth o(*host, root);
    auto at = o.access_token("mock", "sean@example.com");
    check(at.ok() && *at != tok->access_token && counter(base, "token_refresh") == 1, "an expired access token is refreshed through the token endpoint");
    auto again = o.access_token("mock", "sean@example.com");
    check(again.ok() && *again == *at && counter(base, "token_refresh") == 1, "and the fresh one is reused, not refreshed again");
    OAuth o2(*host, root);
    auto persisted = o2.load_token("mock", "sean@example.com");
    check(persisted.ok() && persisted->access_token == *at && persisted->refresh_token == tok->refresh_token, "the refreshed token is saved, with the refresh token kept (the response does not repeat it)");
  }

  // --- revocation by the provider ---
  {
    http_post(base + "/_revoke");
    OAuthToken old = *oauth.load_token("mock", "sean@example.com");
    old.expires_at = 1;
    (void)oauth.save_token(old);
    auto at = oauth.access_token("mock", "sean@example.com");
    check(!at.ok() && at.error().status == Status::Expired, "a revoked sign-in is Expired: the person is told to sign in again");
  }

  // --- the refusals ---
  {
    auto lis2 = hal::net_listen(0, true);
    const std::string red2 = "http://127.0.0.1:" + std::to_string((*lis2)->port()) + "/";
    auto s2 = oauth.begin(*cfg, red2, "sean@example.com");
    std::thread t2([&] { browser(s2->url); });
    auto r2 = oauth.wait_for_redirect(**lis2, 15000);
    t2.join();
    OAuthPending forged = s2->pending;
    forged.state = "someone-elses-state";
    int before = counter(base, "token_auth_code");
    auto a = oauth.finish(*cfg, forged, *r2);
    check(!a.ok() && a.error().status == Status::Denied && counter(base, "token_auth_code") == before, "a redirect whose state is not ours is refused before any network call");
    OAuthPending wrong_verifier = s2->pending;
    wrong_verifier.verifier = std::string(60, 'x');
    auto b = oauth.finish(*cfg, wrong_verifier, *r2);
    check(!b.ok() && b.error().status == Status::Denied, "a wrong PKCE verifier is refused by the token endpoint");
    auto c = oauth.finish(*cfg, s2->pending, *r2);
    check(!c.ok(), "the code was spent by the failed attempt: a code works once");
  }
  {
    auto lis3 = hal::net_listen(0, true);
    const std::string red3 = "http://127.0.0.1:" + std::to_string((*lis3)->port()) + "/";
    auto s3 = oauth.begin(*cfg, red3, "sean@example.com");
    std::thread t3([&] { browser(s3->url + "&deny=1"); });
    auto r3 = oauth.wait_for_redirect(**lis3, 15000);
    t3.join();
    check(r3.ok() && r3->error == "access_denied" && r3->code.empty(), "the person saying no comes back as an error redirect");
    auto f = oauth.finish(*cfg, s3->pending, *r3);
    check(!f.ok() && f.error().status == Status::Denied && f.error().message.find("access_denied") != std::string::npos, "and finish() reports it as Denied with the reason");
  }
  {
    auto lis4 = hal::net_listen(0, true);
    auto r4 = oauth.wait_for_redirect(**lis4, 1200);
    check(!r4.ok() && r4.error().status == Status::Expired, "waiting for a redirect that never comes ends with Expired, not a hang");
    // noise on the listener (a favicon, a probe) is answered and does not end the wait
    const std::string base4 = "http://127.0.0.1:" + std::to_string((*lis4)->port());
    std::thread t4([&] { http_get(base4 + "/favicon.ico"); http_get(base4 + "/?code=abc&state=zzz"); });
    auto r5 = oauth.wait_for_redirect(**lis4, 10000);
    t4.join();
    check(r5.ok() && r5->code == "abc" && r5->state == "zzz", "a favicon request first does not end the wait; the real redirect after it is picked up");
  }

  // --- manual flow: a browser on another machine, the address pasted back, across processes ---
  {
    auto s = oauth.begin(*cfg, "http://127.0.0.1:1/", "sean@example.com");
    check(oauth.save_pending(s->pending).ok(), "a sign-in in progress is saved");
    std::string loc;
    int stt = 0;
    http_get(s->url, &loc, &stt);   // the person's browser; the redirect to 127.0.0.1:1 is not reachable, its address is what they paste
    OAuth other_process(*host, root);
    auto pend = other_process.take_pending("mock");
    check(pend.ok() && pend->state == s->pending.state && pend->verifier == s->pending.verifier, "another process picks it up");
    auto red = OAuth::parse_redirect(loc);
    check(red.ok() && !red->code.empty(), "the pasted address parses");
    auto tk = other_process.finish(*cfg, *pend, *red);
    check(tk.ok() && tk->account == "sean@example.com", "and finishes the sign-in");
    check(!other_process.take_pending("mock").ok(), "a pending sign-in is single use");
    check(!OAuth::parse_redirect("hello").ok(), "pasting something that is not an address is refused with a clear message");
  }

  // --- revoke ---
  {
    OAuthToken t = *oauth.load_token("mock", "sean@example.com");
    auto before = counter(base, "revoked");
    auto rv = oauth.revoke("mock", "sean@example.com");
    check(rv.ok() && counter(base, "revoked") == before + 1 && !oauth.load_token("mock", "sean@example.com").ok(), "revoke() tells the provider and forgets the token");
    (void)t;
  }

  std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
  return g_fail ? 1 : 0;
}
