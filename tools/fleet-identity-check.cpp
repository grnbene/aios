// Stage 1 of docs/design/fleet-identity-keys.md: each kernel root gets its own Ed25519 key in <root>/node.identity.
// Checks: made at first boot, mode 600, the same key after a restart, signatures verify (and a changed message does
// not), and a damaged file is left alone (no silent new identity) with the kernel still booting.
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include "aios/kernel/kernel.hpp"
#include "aios/crypto/curve25519.hpp"
#include "aios/fleet/relay.hpp"

using namespace aios;

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("  [FAIL] %s\n", msg); ++failures; } else std::printf("  [ok] %s\n", msg); } while (0)

static std::string read_all(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main() {
  const std::string root = "/tmp/aios-fleet-identity-check";
  std::system(("rm -rf " + root).c_str());
  const std::string path = root + "/node.identity";
  KernelConfig cfg;
  cfg.root = root;

  crypto::Point32 first{};
  {
    auto k = Kernel::boot(cfg);
    if (!k.ok()) { std::printf("boot failed: %s\n", k.error().message.c_str()); return 1; }
    const crypto::Point32* pub = (*k)->node_public_key();
    CHECK(pub != nullptr, "first boot makes a node identity");
    if (!pub) return 1;
    first = *pub;
    struct stat st{};
    CHECK(::stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "node.identity is mode 600");
    CHECK(read_all(path).size() == 36, "node.identity holds a magic word and a 32-byte seed");
    const std::string msg = "attach challenge 1234";
    auto sig = (*k)->node_sign(span_of(msg));
    CHECK(sig.ok() && crypto::ed25519_verify(span_of(msg), *sig, *pub), "a signature verifies against the public key");
    const std::string other = "attach challenge 1235";
    CHECK(sig.ok() && !crypto::ed25519_verify(span_of(other), *sig, *pub), "the signature does not verify for another message");
  }
  {
    auto k = Kernel::boot(cfg);
    if (!k.ok()) { std::printf("second boot failed: %s\n", k.error().message.c_str()); return 1; }
    const crypto::Point32* pub = (*k)->node_public_key();
    CHECK(pub != nullptr && *pub == first, "a restart keeps the same key");
    // fleet join signs with this key: a hub that lists only the public key (no member key) admits the machine.
    const std::string members = root + "/hub-members";
    { std::ofstream f(members, std::ios::trunc); f << (*k)->self().str() << " ed25519=" << hex(ByteSpan(first.data(), first.size())) << "\n"; }
    RelayHub hub("cluster-key-for-the-check");
    hub.set_members_file(members);
    crypto::Scalar32 hub_seed{}; hub_seed.fill(7);
    const crypto::Point32 hub_pub = crypto::ed25519_public_from_seed(hub_seed);
    hub.set_signer([&](ByteSpan m) { auto s = crypto::ed25519_sign(m, hub_seed, hub_pub); return ByteBuffer(s.begin(), s.end()); });
    auto port = hub.start(0, true);
    CHECK(port.ok(), "a hub starts");
    if (port.ok()) {
      (*k)->set_fleet_hub_key(hub_pub);
      auto j = (*k)->fleet_join("127.0.0.1", *port, "cluster-key-for-the-check");
      CHECK(j.ok() && (*k)->in_fleet(), ("fleet join attaches by its identity key: " + (j.ok() ? std::string("ok") : j.error().message)).c_str());
      bool signed_roster = false;
      for (int i = 0; i < 50 && !signed_roster; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); signed_roster = (*k)->relay() && (*k)->relay()->roster_signed(); }
      std::array<uint8_t, 32> mine{};
      CHECK(signed_roster && (*k)->relay()->peer_key((*k)->self(), mine) && mine == first, "with hub-key pinned, it takes the signed roster (its own key in it)");
    }
    hub.stop();
  }
  {
    // A damaged file: the kernel boots without a key and does not replace it.
    { std::ofstream f(path, std::ios::binary | std::ios::trunc); f << "garbage"; }
    auto k = Kernel::boot(cfg);
    CHECK(k.ok(), "a damaged node.identity does not stop the kernel");
    if (k.ok()) {
      CHECK((*k)->node_public_key() == nullptr, "a damaged node.identity gives no key");
      CHECK(!(*k)->node_key_problem().empty(), "and says why");
      CHECK(!(*k)->node_sign(span_of(std::string("x"))).ok(), "signing without a key fails");
    }
    CHECK(read_all(path) == "garbage", "the damaged file is left as it was");
  }
  std::system(("rm -rf " + root).c_str());
  std::printf(failures ? "fleet identity check: %d failure(s)\n" : "fleet identity check: all passed\n", failures);
  return failures ? 1 : 0;
}
