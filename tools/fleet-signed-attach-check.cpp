// Stage 2 of docs/design/fleet-identity-keys.md: the hub admits a member listed with `ed25519=<hex>` only by a
// signature from that key. Drives the real hub and members over loopback:
//   listed with a key: a member that signs gets in; one that does not sign (an older build), one signing with
//   another key, and one replaying an earlier signature are refused;
//   listed with a member key only, or a hub without a members file: as before, whether or not the member signs;
//   a signing member taken off the members file is dropped within seconds.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include "aios/core/bytes.hpp"
#include "aios/core/ids.hpp"
#include "aios/crypto/curve25519.hpp"
#include "aios/fleet/relay.hpp"

using namespace aios;

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("  [FAIL] %s\n", msg); ++failures; } else std::printf("  [ok] %s\n", msg); } while (0)

static Id128 id_of(uint8_t n) { Id128 id{}; id.hi = 0x5200000000000000ull | n; id.lo = 0x4321ull * n; return id; }

struct Key {
  crypto::Scalar32 seed{};
  crypto::Point32 pub{};
  explicit Key(uint8_t n) { for (size_t i = 0; i < seed.size(); ++i) seed[i] = uint8_t(n * 31 + i); pub = crypto::ed25519_public_from_seed(seed); }
  std::function<ByteBuffer(ByteSpan)> signer() const {
    return [s = seed, p = pub](ByteSpan m) { auto sig = crypto::ed25519_sign(m, s, p); return ByteBuffer(sig.begin(), sig.end()); };
  }
  std::string hex_pub() const { return hex(ByteSpan(pub.data(), pub.size())); }
};

static void write_members(const std::string& path, const std::string& body) { std::ofstream f(path, std::ios::trunc); f << body; }

static std::string attach(uint16_t port, const std::string& cluster, const Id128& id, std::function<ByteBuffer(ByteSpan)> signer,
                          const std::string& member_key = "") {
  RelayClient c(cluster, id);
  if (!member_key.empty()) c.set_member_key(member_key);
  if (signer) c.set_signer(std::move(signer));
  auto r = c.attach("127.0.0.1", port);
  std::string out = r.ok() ? "ok" : r.error().message;
  c.stop();
  return out;
}

int main() {
  const std::string dir = "/tmp/aios-fleet-signed-attach-check";
  std::system(("rm -rf " + dir + " && mkdir -p " + dir).c_str());
  const std::string members = dir + "/members";
  const std::string cluster = "cluster-key-for-the-check";
  Key a(1), b(2), stranger(3);
  const Id128 ida = id_of(1), idb = id_of(2), idc = id_of(3);

  write_members(members, "# check\n" + ida.str() + " ed25519=" + a.hex_pub() + "\n" +
                         idb.str() + " old-member-key\n" +
                         idc.str() + " also-a-key ed25519=" + stranger.hex_pub() + "\n");
  RelayHub hub(cluster);
  hub.set_members_file(members);
  auto port = hub.start(0, true);
  if (!port.ok()) { std::printf("hub did not start\n"); return 1; }
  const uint16_t p = *port;

  std::string r = attach(p, cluster, ida, a.signer());
  CHECK(r == "ok", ("listed with ed25519=, signs with that key: attaches (" + r + ")").c_str());
  r = attach(p, cluster, ida, nullptr, "anything");
  CHECK(r.find("must sign") != std::string::npos, ("listed with ed25519=, does not sign: refused (" + r + ")").c_str());
  r = attach(p, cluster, ida, b.signer());
  CHECK(r.find("bad signature") != std::string::npos, ("listed with ed25519=, signs with another key: refused (" + r + ")").c_str());
  r = attach(p, cluster, idc, nullptr, "also-a-key");
  CHECK(r.find("must sign") != std::string::npos, ("listed with a key and ed25519=: the key alone does not admit (" + r + ")").c_str());
  r = attach(p, cluster, idc, stranger.signer());
  CHECK(r == "ok", ("listed with a key and ed25519=, signs: attaches (" + r + ")").c_str());
  // A key-only member with a wrong key: a signature does not stand in for it.
  r = attach(p, cluster, idb, a.signer(), "wrong");
  CHECK(r.find("bad member key") != std::string::npos, ("a key-only member is checked on its key, a signature does not stand in (" + r + ")").c_str());

  // Replay: a signature captured from one attach, presented on the next.
  ByteBuffer captured;
  auto capture = [&](ByteSpan m) { ByteBuffer s = a.signer()(m); captured = s; return s; };
  r = attach(p, cluster, ida, capture);
  CHECK(r == "ok" && captured.size() == 64, "a first attach (its signature recorded)");
  r = attach(p, cluster, ida, [&](ByteSpan) { return captured; });
  CHECK(r.find("bad signature") != std::string::npos, ("the recorded signature replayed: refused (" + r + ")").c_str());

  // Key-only members are unchanged, signing or not.
  r = attach(p, cluster, idb, nullptr, "old-member-key");
  CHECK(r == "ok", ("listed with a member key only, an older member: attaches (" + r + ")").c_str());
  r = attach(p, cluster, idb, b.signer(), "old-member-key");
  CHECK(r == "ok", ("listed with a member key only, a member that signs: attaches (" + r + ")").c_str());

  // Removing a signing member from the file drops it.
  {
    RelayClient live(cluster, ida);
    live.set_signer(a.signer());
    CHECK(live.attach("127.0.0.1", p).ok(), "a signing member stays attached");
    write_members(members, idb.str() + " old-member-key\n");
    bool gone = false;
    for (int i = 0; i < 80 && !gone; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      gone = true;
      for (const auto& m : hub.members()) if (m == ida) gone = false;
    }
    CHECK(gone, "taken off the members file: dropped within 8 s");
    live.stop();
  }
  hub.stop();

  // A hub without a members file: the cluster key, as before.
  RelayHub open_hub(cluster);
  auto port2 = open_hub.start(0, true);
  if (!port2.ok()) { std::printf("second hub did not start\n"); return 1; }
  r = attach(*port2, cluster, ida, a.signer());
  CHECK(r == "ok", ("no members file, a member that signs: attaches on the cluster key (" + r + ")").c_str());
  r = attach(*port2, "not-the-key", ida, a.signer());
  CHECK(r != "ok", "no members file, the wrong cluster key: refused, signature or not");
  open_hub.stop();

  std::system(("rm -rf " + dir).c_str());
  std::printf(failures ? "fleet signed attach check: %d failure(s)\n" : "fleet signed attach check: all passed\n", failures);
  return failures ? 1 : 0;
}
