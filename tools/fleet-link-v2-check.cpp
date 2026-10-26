// Stage 3 of docs/design/fleet-identity-keys.md.
// The link identity exchange, on real loopback sockets after the real token handshake:
//   both sides newer and listed: rekeyed (fresh key each time, not the token session's), frames flow both ways;
//   a peer signing with a key other than the roster's, or not signing while listed: refused;
//   a listed peer that does not offer the exchange (an older build, or the offer stripped): refused;
//   an unlisted older peer: the token link as before - unless this side is v2-only;
//   neither side listed: still rekeyed from X25519 (recorded traffic stays closed).
// The signed roster through a real hub: a member that pinned the hub key gets each peer's key and roles; a wrong
// pinned key or none gets no keys; a key changed in the members file reaches members within seconds.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "aios/core/bytes.hpp"
#include "aios/core/ids.hpp"
#include "aios/crypto/curve25519.hpp"
#include "aios/fleet/relay.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/net.hpp"
#include "aios/node/secure.hpp"

using namespace aios;

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("  [FAIL] %s\n", std::string(msg).c_str()); ++failures; } else std::printf("  [ok] %s\n", std::string(msg).c_str()); } while (0)

static Id128 id_of(uint8_t n) { Id128 id{}; id.hi = 0x5300000000000000ull | n; id.lo = 0x9876ull * n; return id; }

struct Key {
  crypto::Scalar32 seed{};
  crypto::Point32 pub{};
  explicit Key(uint8_t n) { for (size_t i = 0; i < seed.size(); ++i) seed[i] = uint8_t(n * 17 + i); pub = crypto::ed25519_public_from_seed(seed); }
  std::function<ByteBuffer(ByteSpan)> signer() const {
    return [s = seed, p = pub](ByteSpan m) { auto sig = crypto::ed25519_sign(m, s, p); return ByteBuffer(sig.begin(), sig.end()); };
  }
};

static ByteBuffer features(bool identity) {
  ByteBuffer b; Writer w(b);
  w.u32(0x54414546u);                            // 'FEAT'
  w.u32((1u << 1) | (identity ? (1u << 2) : 0));  // deflate, identity
  return b;
}

// One side's view: its signing key (or none), the peers it lists, v2-only, and whether it offers the exchange.
struct Side {
  std::shared_ptr<LinkIdentity> id = std::make_shared<LinkIdentity>();
  bool offers = true;
  void sign_with(const Key& k) { id->sign = k.signer(); }
  void list(const Id128& peer, const crypto::Point32& pk) {
    auto prev = id->peer_key;
    id->peer_key = [prev, peer, pk](const Id128& p, std::array<uint8_t, 32>& out) {
      if (p == peer) { out = pk; return true; }
      return prev ? prev(p, out) : false;
    };
  }
};

struct Outcome {
  bool ok_i = false, ok_r = false, up_i = false, up_r = false;
  std::string err_i, err_r;
  crypto::Key token_i{}, key_i{}, key_r{};
  bool talk = false;   // a frame each way over the final links
};

static Outcome run(Side& ini, Side& res) {
  Outcome o;
  auto host = hal::make_host();
  auto l = hal::net_listen(0, true);
  if (!l.ok()) { o.err_i = "listen failed"; return o; }
  const uint16_t port = (*l)->port();
  const Id128 me_i = id_of(1), me_r = id_of(2);
  const std::string token = "token-for-the-link-v2-check";
  std::thread rt([&] {
    std::unique_ptr<hal::Connection> c;
    for (int i = 0; i < 50 && !c; ++i) { auto a = (*l)->accept(200); if (a.ok()) c = std::move(*a); }
    if (!c) { o.err_r = "no connection"; return; }
    auto sc = std::shared_ptr<hal::Connection>(std::move(c));
    auto plain = make_plain_link(sc);
    Id128 peer; ByteBuffer pp; const ByteBuffer mine = features(res.offers);
    auto k = secure_handshake_responder(*plain, token, *host, me_r, peer, pp, ByteSpan(mine.data(), mine.size()));
    if (!k.ok()) { o.err_r = k.error().message; return; }
    auto link = make_secure_link(sc, *k, false);
    bool up = false; crypto::Key k2{};
    // An older responder never runs the exchange: it just carries on with the token link.
    auto x = res.offers ? link_identity_exchange(*link, *k, false, me_r, peer, ByteSpan(pp.data(), pp.size()), *host, res.id.get(), up, k2) : Result<void>(Ok());
    if (!x.ok()) { o.err_r = x.error().message; sc->close(); return; }
    o.ok_r = true; o.up_r = up; o.key_r = up ? k2 : *k;
    if (up) link = make_secure_link(sc, k2, false);
    uint8_t t; ByteBuffer p;
    if (link->recv(t, p, 5000) && t == 0x10) link->send(0x11, ByteSpan(p.data(), p.size()));
  });
  auto c = hal::net_connect("127.0.0.1", port, 5000);
  if (!c.ok()) { o.err_i = "connect failed"; rt.join(); return o; }
  auto sc = std::shared_ptr<hal::Connection>(std::move(*c));
  auto plain = make_plain_link(sc);
  Id128 peer; ByteBuffer pp; const ByteBuffer mine = features(ini.offers);
  auto k = secure_handshake_initiator(*plain, token, *host, me_i, peer, pp, ByteSpan(mine.data(), mine.size()));
  if (!k.ok()) { o.err_i = k.error().message; sc->close(); rt.join(); return o; }
  o.token_i = *k;
  auto link = make_secure_link(sc, *k, true);
  bool up = false; crypto::Key k2{};
  auto x = ini.offers ? link_identity_exchange(*link, *k, true, me_i, peer, ByteSpan(pp.data(), pp.size()), *host, ini.id.get(), up, k2) : Result<void>(Ok());
  if (!x.ok()) { o.err_i = x.error().message; sc->close(); rt.join(); return o; }
  o.ok_i = true; o.up_i = up; o.key_i = up ? k2 : *k;
  if (up) link = make_secure_link(sc, k2, true);
  const std::string hello = "ping over the final link";
  if (link->send(0x10, span_of(hello))) {
    uint8_t t; ByteBuffer p;
    o.talk = link->recv(t, p, 5000) && t == 0x11 && std::string(p.begin(), p.end()) == hello;
  }
  sc->close();
  rt.join();
  return o;
}

int main() {
  const Key ka(1), kb(2), other(3);
  const Id128 ida = id_of(1), idb = id_of(2);
  std::printf("link identity exchange\n");
  {
    Side i, r; i.sign_with(ka); r.sign_with(kb); i.list(idb, kb.pub); r.list(ida, ka.pub);
    Outcome o = run(i, r);
    CHECK(o.ok_i && o.ok_r && o.up_i && o.up_r, "both listed, both sign: the link is rekeyed (" + o.err_i + o.err_r + ")");
    CHECK(o.key_i == o.key_r, "both ends hold the same new key");
    CHECK(o.key_i != o.token_i, "the new key is not the token session's");
    CHECK(o.talk, "frames flow both ways on the rekeyed link");
    Outcome o2 = run(i, r);
    CHECK(o2.ok_i && o2.key_i != o.key_i, "a second link gets a different key");
  }
  {
    Side i, r; i.sign_with(ka); r.sign_with(other); i.list(idb, kb.pub); r.list(ida, ka.pub);
    Outcome o = run(i, r);
    CHECK(!o.ok_i && o.err_i.find("roster key") != std::string::npos, "the responder signs with another key: the initiator refuses (" + o.err_i + ")");
  }
  {
    Side i, r; r.sign_with(kb); i.list(idb, kb.pub); r.list(ida, ka.pub);   // the initiator has no key to sign with
    Outcome o = run(i, r);
    CHECK(!o.ok_r && o.err_r.find("did not sign") != std::string::npos, "a listed initiator that does not sign: the responder refuses (" + o.err_r + ")");
    CHECK(!o.talk, "and nothing passes over that link");
  }
  {
    Side i, r; i.sign_with(ka); i.list(idb, kb.pub); r.offers = false;
    Outcome o = run(i, r);
    CHECK(!o.ok_i && o.err_i.find("did not offer") != std::string::npos, "a listed peer that does not offer the exchange: refused (" + o.err_i + ")");
  }
  {
    Side i, r; i.sign_with(ka); r.offers = false;
    Outcome o = run(i, r);
    CHECK(o.ok_i && !o.up_i && o.talk, "an unlisted older peer: the token link as before (" + o.err_i + ")");
    i.id->v2_only = true;
    Outcome o2 = run(i, r);
    CHECK(!o2.ok_i && o2.err_i.find("v2-only") != std::string::npos, "the same peer to a v2-only machine: refused (" + o2.err_i + ")");
  }
  {
    Side i, r; i.sign_with(ka); r.sign_with(kb); i.id->v2_only = true;   // the initiator does not list the responder
    Outcome o = run(i, r);
    CHECK(!o.ok_i && o.err_i.find("not in the signed roster") != std::string::npos, "v2-only and the peer is not in the roster: refused (" + o.err_i + ")");
  }
  {
    Side i, r;   // no keys, no roster
    Outcome o = run(i, r);
    CHECK(o.ok_i && o.ok_r && o.up_i && o.key_i == o.key_r && o.key_i != o.token_i && o.talk, "no keys anywhere: still rekeyed from X25519 (" + o.err_i + o.err_r + ")");
  }

  std::printf("signed roster through the hub\n");
  {
    const std::string dir = "/tmp/aios-fleet-link-v2-check";
    std::system(("rm -rf " + dir + " && mkdir -p " + dir).c_str());
    const std::string members = dir + "/members";
    const Key hubk(9), kc(4);
    const Id128 ma = id_of(11), mb = id_of(12);
    auto write = [&](const crypto::Point32& bkey) {
      std::ofstream f(members, std::ios::trunc);
      f << ma.str() << " ed25519=" << hex(ByteSpan(ka.pub.data(), 32)) << " roles=app-route,sql\n";
      f << mb.str() << " ed25519=" << hex(ByteSpan(bkey.data(), 32)) << "\n";
    };
    write(kb.pub);
    const std::string cluster = "cluster-key-for-the-v2-check";
    RelayHub hub(cluster);
    hub.set_members_file(members);
    hub.set_signer(hubk.signer());
    auto port = hub.start(0, true);
    CHECK(port.ok(), "a hub starts");
    if (!port.ok()) return 1;
    RelayClient a(cluster, ma), b(cluster, mb), stranger_pin(cluster, ma), unpinned(cluster, ma);
    a.set_signer(ka.signer()); a.set_hub_key(hubk.pub);
    b.set_signer(kb.signer()); b.set_hub_key(hubk.pub);
    CHECK(a.attach("127.0.0.1", *port).ok() && b.attach("127.0.0.1", *port).ok(), "two signing members attach");
    std::array<uint8_t, 32> got{};
    bool seen = false;
    for (int i = 0; i < 50 && !seen; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); seen = a.peer_key(mb, got); }
    CHECK(seen && got == kb.pub, "a member that pinned the hub key has its peer's key from the signed roster");
    CHECK(a.roster_signed() && b.peer_roles(ma) == "app-route,sql", "and its roles (" + b.peer_roles(ma) + ")");
    a.stop();
    // A member with the wrong pinned key, and one with none: no keys taken.
    stranger_pin.set_signer(ka.signer()); stranger_pin.set_hub_key(other.pub);
    unpinned.set_signer(ka.signer());
    CHECK(stranger_pin.attach("127.0.0.1", *port).ok(), "a member pinning another hub key still attaches");
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    CHECK(!stranger_pin.roster_signed() && !stranger_pin.peer_key(mb, got), "but takes no keys from a roster that key did not sign");
    stranger_pin.stop();
    CHECK(unpinned.attach("127.0.0.1", *port).ok(), "a member with no pinned key attaches");
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    CHECK(!unpinned.roster_signed() && !unpinned.peer_key(mb, got), "and takes no keys");
    // b's key changes in the members file; a (re-attached) sees the new one without reconnecting.
    RelayClient a2(cluster, ma);
    a2.set_signer(ka.signer()); a2.set_hub_key(hubk.pub);
    unpinned.stop();
    CHECK(a2.attach("127.0.0.1", *port).ok(), "a member re-attaches");
    write(kc.pub);
    bool changed = false;
    for (int i = 0; i < 80 && !changed; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); changed = a2.peer_key(mb, got) && got == kc.pub; }
    CHECK(changed, "a key changed in the members file reaches members within 8 s");
    a2.stop(); b.stop();
    hub.stop();
    std::system(("rm -rf " + dir).c_str());
  }
  std::printf(failures ? "fleet link v2 check: %d failure(s)\n" : "fleet link v2 check: all passed\n", failures);
  return failures ? 1 : 0;
}
