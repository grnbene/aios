// Stage 4 of docs/design/fleet-identity-keys.md: a node enforces the roles the signed roster gives a peer.
// A real kernel serves node links; a peer connects with an identity key the roster (as this node sees it) lists:
//   roles=app-route: an app.route operation and memory staging pass the role check; compute, app admin, shell and
//   SQL are refused; telemetry is open;
//   roles=app-admin,sql: `app ...` command lines and SQL run; other command lines (shell) are refused;
//   no roles listed: today's access;
//   a peer not in the roster (roles offered for it but no key): not limited by them.
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "aios/compute/compute_graph.hpp"
#include "aios/fleet/relay.hpp"
#include "aios/core/bytes.hpp"
#include "aios/crypto/curve25519.hpp"
#include "aios/hal/net.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/node/node.hpp"
#include "aios/node/secure.hpp"

using namespace aios;

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("  [FAIL] %s\n", std::string(msg).c_str()); ++failures; } else std::printf("  [ok] %s\n", std::string(msg).c_str()); } while (0)

// nodeproto frame types (src/node/protocol.hpp)
constexpr uint8_t kMemAlloc = 2, kExec = 6;

static bool refused_for_role(const std::string& msg) { return msg.find(" role here") != std::string::npos; }

int main() {
  const std::string root = "/tmp/aios-fleet-roles-check";
  { std::error_code fs_err; std::filesystem::remove_all(root, fs_err); }
  KernelConfig cfg;
  cfg.root = root;
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("boot failed: %s\n", k.error().message.c_str()); return 1; }
  const std::string token = "token-for-the-roles-check";
  auto port = (*k)->serve(0, token, false, true);
  if (!port.ok()) { std::printf("serve failed: %s\n", port.error().message.c_str()); return 1; }
  (*k)->service()->set_command_runner([](const std::string& line, std::string& out) { out = "ran: " + line; return 0; });
  (*k)->service()->set_sql_runner([](const std::string&) -> Result<ByteBuffer> { return ByteBuffer{'o', 'k'}; });

  // The peer's key, and what the roster says of it. Peer and node share this process, so one identity serves both
  // ends: the node checks the peer against it; the peer signs with it (and the node itself is not listed).
  crypto::Scalar32 seed{}; seed.fill(5);
  const crypto::Point32 pub = crypto::ed25519_public_from_seed(seed);
  Id128 peer{}; peer.hi = 0x5400000000000000ull; peer.lo = 77;
  std::string roster_roles;
  bool listed = true;
  auto id = std::make_shared<LinkIdentity>();
  id->sign = [&](ByteSpan m) { auto s = crypto::ed25519_sign(m, seed, pub); return ByteBuffer(s.begin(), s.end()); };
  id->peer_key = [&](const Id128& p, std::array<uint8_t, 32>& out) { if (!listed || !(p == peer)) return false; out = pub; return true; };
  id->peer_roles = [&](const Id128& p) { return p == peer ? roster_roles : std::string(); };
  set_link_identity(id);

  auto host = hal::make_host();
  auto connect = [&]() -> std::shared_ptr<RemoteNode> {
    auto c = hal::net_connect("127.0.0.1", *port, 5000);
    if (!c.ok()) return nullptr;
    auto rn = std::make_shared<RemoteNode>(std::shared_ptr<hal::Connection>(std::move(*c)), "127.0.0.1");
    auto h = rn->hello(token, peer, *host);
    if (!h.ok()) { std::printf("  hello failed: %s\n", h.error().message.c_str()); return nullptr; }
    return rn;
  };
  auto exec_of = [](OpCode code, uint64_t custom) {
    Operation op; op.code = code; op.custom_kernel = custom;
    ByteBuffer b; Writer w(b); op.encode(w); return b;
  };
  const ByteBuffer app_route = exec_of(OpCode::Custom, fnv1a(std::string("app.route")));
  const ByteBuffer compute = exec_of(OpCode::Nop, 0);
  ByteBuffer alloc; { Writer w(alloc); w.u64(64); }

  std::printf("roles=app-route\n");
  roster_roles = "app-route";
  if (auto rn = connect()) {
    auto e = rn->call(kExec, ByteSpan(app_route.data(), app_route.size()));
    CHECK(e.ok() || !refused_for_role(e.error().message), "an app.route operation passes the role check (" + (e.ok() ? std::string("ran") : e.error().message) + ")");
    auto m = rn->call(kMemAlloc, ByteSpan(alloc.data(), alloc.size()));
    CHECK(m.ok(), "memory staging for it is allowed");
    auto x = rn->call(kExec, ByteSpan(compute.data(), compute.size()));
    CHECK(!x.ok() && refused_for_role(x.error().message), "other compute: refused (" + (x.ok() ? std::string("ran") : x.error().message) + ")");
    auto a = rn->command("app status");
    CHECK(!a.ok() && refused_for_role(a.error().message), "app admin: refused (" + (a.ok() ? a->second : a.error().message) + ")");
    auto sh = rn->command("uname -a");
    CHECK(!sh.ok() && refused_for_role(sh.error().message), "shell: refused");
    auto q = rn->query("SELECT 1");
    CHECK(!q.ok() && refused_for_role(q.error().message), "SQL: refused");
    CHECK(rn->telemetry().ok(), "telemetry: open");
  } else CHECK(false, "the peer connects");

  std::printf("roles=app-admin,sql\n");
  roster_roles = "app-admin,sql";
  if (auto rn = connect()) {
    auto a = rn->command("app status");
    CHECK(a.ok() && a->second == "ran: app status", "an app command runs (" + (a.ok() ? a->second : a.error().message) + ")");
    auto sh = rn->command("uname -a");
    CHECK(!sh.ok() && refused_for_role(sh.error().message), "a shell command: refused (" + (sh.ok() ? sh->second : sh.error().message) + ")");
    auto q = rn->query("SELECT 1");
    CHECK(q.ok(), "SQL runs");
    auto x = rn->call(kExec, ByteSpan(compute.data(), compute.size()));
    CHECK(!x.ok() && refused_for_role(x.error().message), "compute: refused");
  } else CHECK(false, "the peer connects");

  std::printf("no roles listed\n");
  roster_roles = "";
  if (auto rn = connect()) {
    auto sh = rn->command("uname -a");
    CHECK(sh.ok() && sh->second == "ran: uname -a", "today's access: a command runs");
    auto x = rn->call(kExec, ByteSpan(compute.data(), compute.size()));
    CHECK(x.ok() || !refused_for_role(x.error().message), "and compute is not refused for a role");
  } else CHECK(false, "the peer connects");

  std::printf("a peer not in the roster\n");
  roster_roles = "app-route";
  listed = false;
  if (auto rn = connect()) {
    auto sh = rn->command("uname -a");
    CHECK(sh.ok(), "roles bind only to a peer proven by its roster key: not limited (" + (sh.ok() ? sh->second : sh.error().message) + ")");
  } else CHECK(false, "the peer connects");

  std::printf("a member's relay command server (roles=app-admin)\n");
  {
    listed = true;
    roster_roles = "app-admin";
    const std::string cluster = "cluster-key-for-the-roles-check";
    RelayHub hub(cluster);
    auto hp = hub.start(0, true);
    Id128 server_id{}; server_id.hi = 0x5400000000000000ull; server_id.lo = 99;
    RelayClient server(cluster, server_id), client(cluster, peer);
    const bool up = hp.ok() && server.attach("127.0.0.1", *hp).ok() && client.attach("127.0.0.1", *hp).ok();
    // After attaching: the command server runs while its member is attached.
    server.set_command_runner([](const std::string& line, std::string& out) { out = "ran: " + line; return 0; });
    CHECK(up, "a hub and two members");
    std::shared_ptr<RemoteNode> rn;
    if (up) {
      auto c = client.open(server_id);
      if (c.ok()) {
        rn = std::make_shared<RemoteNode>(std::shared_ptr<hal::Connection>(std::move(*c)), "relay");
        auto h = rn->hello(cluster, peer, *host);
        if (!h.ok()) { std::printf("  hello failed: %s\n", h.error().message.c_str()); rn.reset(); }
      }
    }
    if (rn) {
      auto a = rn->command("app status");
      CHECK(a.ok() && a->second == "ran: app status", "an app command runs (" + (a.ok() ? a->second : a.error().message) + ")");
      auto sh = rn->command("uname -a");
      CHECK(!sh.ok() && refused_for_role(sh.error().message), "a shell command: refused (" + (sh.ok() ? sh->second : sh.error().message) + ")");
    } else CHECK(false, "a circuit to the command server");
    rn.reset();
    client.stop(); server.stop(); hub.stop();
  }

  set_link_identity(nullptr);
  (*k).reset();
  { std::error_code fs_err; std::filesystem::remove_all(root, fs_err); }
  std::printf(failures ? "fleet roles check: %d failure(s)\n" : "fleet roles check: all passed\n", failures);
  return failures ? 1 : 0;
}
