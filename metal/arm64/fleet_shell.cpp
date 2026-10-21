/* The real seam between "src/fleet::RelayClient links" and "this Pi is
 * visible on the fleet": a shell command that actually dials a hub and
 * reports the roster it gets back - not a link test, the real thing. */
#include "aios/fleet/relay.hpp"
#include "aios/hal/net.hpp"
#include "aios/node/node.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/hal/host.hpp"
#include "aios/userland/machine_sql.hpp"
#include "kernel_boot.hpp"
#include "aios/core/ids.hpp"
#include "kio.hpp"
#include "fat32.hpp"
#include "net.hpp"
#include "shell.hpp"
#include "fleet_view.hpp"
#include <cstdlib>
#include <string>
#include <vector>

extern "C" void crashlog(const char* line);

namespace {
uint64_t fleet_now_ms();
extern "C" bool arm64_kernel_ready();
// A fresh identity per attempt is fine for proving the wire protocol works;
// a real deployment persists this the way Kernel::boot() does for the
// hosted/x86-bare-metal kernels; this substrate has no persisted identity
// store yet; is committed to memory, not on this pass.
aios::Id128 arm64_generate_self_id() {
  uint64_t cnt;
  asm volatile("mrs %0, cntpct_el0" : "=r"(cnt));
  aios::IdGenerator ids(0xA5A5A5A5A5A5A5A5ull ^ reinterpret_cast<uint64_t>(&ids) ^ (cnt * 0x9E3779B97F4A7C15ull), 0);
  return ids.next();
}

/* This board's fleet identity is its kernel's own (kernel.identity on the
 * card, kept across reboots) - what a hosted Kernel::fleet_join() uses, and
 * what the node handshake presents. A separate fleet ID (FLEET.ID, briefly)
 * made peers look the board up under one identity and meet another in the
 * handshake: every survey opened a fresh circuit and the observer's list
 * grew by one Pi per sweep. */
aios::Id128 board_identity() {
  return aios_arm64::kernel().self();
}

// Deliberately never freed: this pointer's whole job is to keep the
// RelayClient (and the hub connection it owns) alive for as long as the
// kernel runs. The very first version of this command built a LOCAL
// RelayClient and called stop() on it right after reporting success -
// which detached from the hub the instant the function returned, so the
// hub's own roster never had a chance to show this node before it was
// gone again. Staying registered is the entire point of joining.
aios::RelayClient* g_fleet_client = nullptr;
/* Serves the circuits fleet peers open to this board through the hub - the
 * handshake and the node protocol (telemetry, memory, exec). Without it the
 * Pi was on the hub's roster but answered nothing, so a peer's survey (the
 * console's fleet observer) never counted it. */
aios::NodeService* g_fleet_node = nullptr;

/* A fleet peer's Command: one console command, run here with its output
 * captured - the console, reached through the cluster key and the node
 * link's end-to-end encryption instead of an open UDP port. Refused: `fleet`
 * (it would tear down the very link carrying the reply) and the desktop and
 * dashboard (they never return, and the link's heartbeat stops with them). */
int fleet_run_command(const std::string& line, std::string& output) {
  const char* cmd = line.c_str();
  while (*cmd == ' ') ++cmd;
  auto word = [cmd](const char* w) {
    size_t n = 0;
    while (w[n]) { if (cmd[n] != w[n]) return false; ++n; }
    return cmd[n] == 0 || cmd[n] == ' ';
  };
  /* `fleet survey` is read-only (board_reach()/telemetry() to the rest of
   * the roster) - it never joins/leaves/touches THIS link, so letting it
   * through here is how the Survey freeze got reproduced and timed from the
   * host instead of guessed at from a frozen screen. Every other `fleet`
   * subcommand stays refused: join/leave really would cut the link this
   * reply has to come back over. */
  const bool is_survey = word("fleet") && (line.size() > 6) && line.compare(6, 6, "survey") == 0;
  if (!is_survey && (word("fleet") || word("desktop") || word("gui") || word("dash"))) {
    output = std::string("`") + line + "` cannot run over the fleet link (it would cut the link, or never return)\n";
    return 2;
  }
  std::vector<char> buf(256 * 1024);
  kconsole_capture_start(buf.data(), buf.size());
  dispatch_shell_command(cmd);
  const size_t n = kconsole_capture_stop();
  output.assign(buf.data(), n < buf.size() ? n : buf.size() - 1);
  const bool unknown = output.size() >= 13 && output.compare(output.size() - 13, 13, "? try 'help'\n") == 0;
  return unknown ? 127 : 0;
}

/* Outbound: this board reaching a fleet peer (sql_on from a grain program
 * here). The hub link lives here, not in the Kernel, so the kernel's own
 * reach() has nothing to open circuits with. The key is kept for the node
 * handshake each new peer link runs; one link per peer, reused while it
 * lives. */
std::string g_fleet_key;
std::vector<std::pair<aios::Id128, std::shared_ptr<aios::RemoteNode>>> g_peer_links;

aios::Result<std::shared_ptr<aios::RemoteNode>> board_reach(aios::Kernel& k, const std::string& want) {
  using R = aios::Result<std::shared_ptr<aios::RemoteNode>>;
  if (!g_fleet_client || !g_fleet_client->attached()) return R(aios::Status::Busy, "this board is not on a fleet hub");
  std::vector<aios::Id128> hits;
  for (auto& id : g_fleet_client->roster()) if (id.str().compare(0, want.size(), want) == 0) hits.push_back(id);
  if (hits.size() != 1) return R(aios::Status::NotFound, (hits.empty() ? "no fleet member starts with " : "more than one fleet member starts with ") + want);
  for (auto& pl : g_peer_links) if (pl.first == hits[0] && pl.second->alive()) return pl.second;
  auto c = g_fleet_client->open(hits[0]);
  if (!c.ok()) return c.error();
  auto node = std::make_shared<aios::RemoteNode>(std::shared_ptr<aios::hal::Connection>(std::move(*c)), "relay:" + hits[0].short_str());
  static std::unique_ptr<aios::hal::Host> host = aios::hal::make_host();
  auto h = node->hello(g_fleet_key, k.self(), *host);
  if (!h.ok()) return h.error();
  for (auto& pl : g_peer_links) if (pl.first == hits[0]) { pl.second = node; return node; }
  g_peer_links.emplace_back(hits[0], node);
  return node;
}
}  // namespace

extern "C" void fleet_join_test(const char* args) {
  while (*args == ' ') ++args;

  /* `fleet survey`: the dashboard's Survey button, driven from the console
   * instead of a physical click - so it can be reproduced and timed from a
   * script (grain_remote.py --fleet) instead of guessing from a frozen
   * screen. */
  if (args[0] == 's' && args[1] == 'u' && args[2] == 'r' && args[3] == 'v' && args[4] == 'e' && args[5] == 'y' && (args[6] == '\0' || args[6] == ' ')) {
    if (!g_fleet_client || !g_fleet_client->attached()) { kputs("  fleet: not attached - join first\n"); return; }
    FleetMember members[16];
    const uint64_t t0 = fleet_now_ms();
    const size_t n = fleet_survey(members, 16);
    const uint64_t elapsed = fleet_now_ms() - t0;
    kprintf("  fleet: surveyed %llu member(s) in %llu ms\n", static_cast<unsigned long long>(n), static_cast<unsigned long long>(elapsed));
    for (size_t i = 0; i < n; ++i) {
      if (members[i].reached) kprintf("    - %s: ok, rtt=%llu ms, cpus=%u slots=%u jobs=%u mem=%llu/%llu dbs=%u\n", members[i].id, static_cast<unsigned long long>(members[i].rtt_ms), members[i].cpus, members[i].slots, members[i].jobs, static_cast<unsigned long long>(members[i].memory_in_use), static_cast<unsigned long long>(members[i].total_memory), members[i].databases);
      else kprintf("    - %s: %s\n", members[i].id, members[i].note);
    }
    return;
  }

  if (*args == '\0') {
    if (!g_fleet_client) { kputs("  fleet: not attached. usage: fleet <hub_host_ip> <hub_port> <cluster_key>\n"); return; }
    kprintf("  fleet: attached=%s to %s\n", g_fleet_client->attached() ? "yes" : "no", g_fleet_client->hub_address().c_str());
    auto roster = g_fleet_client->roster();
    kprintf("  fleet: roster (excluding self): %llu peer(s)\n", static_cast<unsigned long long>(roster.size()));
    for (auto& id : roster) kprintf("    - %s\n", id.short_str().c_str());
    return;
  }

  char host[64] = {};
  unsigned port = 0;
  char key[128] = {};
  int n = 0;
  {
    const char* p = args;
    while (*p && *p != ' ' && n < 63) host[n++] = *p++;
    host[n] = 0;
    while (*p == ' ') ++p;
    while (*p >= '0' && *p <= '9') port = port * 10 + static_cast<unsigned>(*p++ - '0');
    while (*p == ' ') ++p;
    int m = 0;
    while (*p && m < 127) key[m++] = *p++;
    key[m] = 0;
  }
  if (host[0] == 0 || port == 0 || key[0] == 0) {
    kputs("  usage: fleet <hub_host_ip> <hub_port> <cluster_key | @FILE>   (or `fleet` alone to check status)\n");
    return;
  }
  /* @FILE: the key from a file on the SD card. A key typed on the console
   * line is a secret on the wire and, before the console stopped logging
   * this command's arguments, on the screen's network log. */
  if (key[0] == '@') {
    static char kbuf[256];
    size_t n = 0;
    if (!fat32_read_file(key + 1, kbuf, sizeof(kbuf) - 1, n) || n == 0) { kprintf("  fleet: cannot read the key file %s\n", key + 1); return; }
    while (n > 0 && (kbuf[n - 1] == '\n' || kbuf[n - 1] == '\r' || kbuf[n - 1] == ' ')) --n;
    kbuf[n] = 0;
    if (n >= sizeof(key)) { kputs("  fleet: key file holds more than 127 characters\n"); return; }
    for (size_t i = 0; i <= n; ++i) key[i] = kbuf[i];
    for (size_t i = 0; i < sizeof(kbuf); ++i) kbuf[i] = 0;
  }
  /* A new hub replaces the old link (it used to refuse until a reboot - a
   * board left on a hub that had gone away had no way off it). */
  if (g_fleet_client) {
    kprintf("  fleet: leaving %s\n", g_fleet_client->hub_address().c_str());
    aios::RelayClient* old = g_fleet_client;
    aios::NodeService* old_node = g_fleet_node;
    g_fleet_client = nullptr;                 /* fleet_poll stops touching it first */
    g_fleet_node = nullptr;
    g_peer_links.clear();                     /* their circuits rode the old hub link */
    old->stop();
    delete old;
    if (old_node) { old_node->stop(); delete old_node; }
  }

  aios::Id128 self = board_identity();
  kprintf("  fleet: this board's identity: %s\n", self.short_str().c_str());
  kprintf("  fleet: dialing hub %s:%u ...\n", host, static_cast<uint64_t>(port));

  /* Polled, not threaded: attach() hands its reader loop to a hal::Thread,
   * and with no free core that body runs inline and never returns - the
   * console command hung while the link itself worked. net_poll() drives
   * this one (fleet_poll below). */
  auto* client = new aios::RelayClient(std::string(key), self);
  auto* node = new aios::NodeService(aios_arm64::kernel(), std::string(key));
  node->set_command_runner(fleet_run_command);
  aios::userland::serve_machine_sql(*node, aios_arm64::kernel());
  aios::userland::set_peer_reach(board_reach);
  aios::userland::set_peer_roster([](aios::Kernel&) {
    return g_fleet_client && g_fleet_client->attached() ? g_fleet_client->roster() : std::vector<aios::Id128>{};
  });
  g_fleet_key = key;
  for (char& ch : key) ch = 0;
  auto r = client->attach_polled(std::string(host), static_cast<uint16_t>(port));
  if (!r.ok()) {
    kprintf("  fleet: attach FAILED: %s\n", r.error().str().c_str());
    delete client;
    delete node;
    return;
  }
  g_fleet_client = client;
  g_fleet_node = node;
  kputs("  fleet: ATTACHED - this Pi is now registered on the hub, and stays that way.\n");
  auto roster = client->roster();
  kprintf("  fleet: roster (excluding self): %llu peer(s)\n", static_cast<unsigned long long>(roster.size()));
  for (auto& id : roster) kprintf("    - %s\n", id.short_str().c_str());
}

namespace {
/* Joining at boot: FLEET.CFG on the card holds exactly what `fleet` takes
 * (`<hub-ip> 7420 @FLEET.KEY`). Until it existed every reboot left the
 * board off the fleet until someone typed the command again. Tried once eth0
 * has a lease, then on a backoff until it is in - a hub that is down at boot
 * must not leave the board out for good. Once attached, the relay client's
 * own reconnect keeps it there. */
bool g_auto_done = false;
uint64_t g_auto_next_ms = 0;
uint32_t g_auto_backoff_ms = 10000;

uint64_t fleet_now_ms() {
  uint64_t cnt, frq;
  asm volatile("mrs %0, cntpct_el0" : "=r"(cnt));
  asm volatile("mrs %0, cntfrq_el0" : "=r"(frq));
  return frq ? cnt / (frq / 1000) : 0;
}

void fleet_autojoin_step() {
  if (g_auto_done || g_fleet_client) return;
  if (!fat32_status().mounted || !net_is_dhcp_bound(NetIfType::Ethernet)) return;
  const uint64_t now = fleet_now_ms();
  if (now < g_auto_next_ms) return;
  char cfg[160];
  size_t n = 0;
  if (!fat32_read_file("FLEET.CFG", cfg, sizeof(cfg) - 1, n) || n == 0) { g_auto_done = true; return; }
  while (n > 0 && (cfg[n - 1] == '\n' || cfg[n - 1] == '\r' || cfg[n - 1] == ' ')) --n;
  cfg[n] = 0;
  kputs("  fleet: joining from FLEET.CFG\n");
  fleet_join_test(cfg);
  if (g_fleet_client) { g_auto_done = true; return; }
  g_auto_next_ms = now + g_auto_backoff_ms;
  kprintf("  fleet: FLEET.CFG join failed, retrying in %u s\n", static_cast<uint64_t>(g_auto_backoff_ms / 1000));
  if (g_auto_backoff_ms < 60000) g_auto_backoff_ms *= 2;
}
}  // namespace

/* Called from the outermost net_poll(): the link's heartbeat and frames. */
/* The Things' clock on this board (it has no thread for one): about once a
 * second, the waits whose moment has come, the events now due, mirrors' pulls
 * and alerts - only when the SQL engine is already open. Called from the frame
 * loops, OUTSIDE net_poll(): a pull waits on fleet peers, and a wait nested in
 * net_poll() never runs fleet_poll() (it only runs at depth 1), so the hub
 * link went unread for the whole pull - the hub's frames backed up and, before
 * the hub queued its writes, stopped the fleet (2026-09-27). Out here a pull's
 * waits spin net_poll() at depth 1 and the link is read as they go, the way
 * the dashboard's Survey already worked. */
extern "C" void things_frame_tick() {
  static bool busy = false;
  if (busy) return;
  static uint64_t next_tick_ms = 0;
  const uint64_t now = fleet_now_ms();
  if (now < next_tick_ms) return;
  next_tick_ms = now + 1000;
  // (never booting the kernel for it: kernel() boots one on first call)
  if (!arm64_kernel_ready()) return;
  auto* db = aios::userland::machine_sql_if_open(aios_arm64::kernel());
  if (!db) return;
  busy = true;
  db->things_tick();
  busy = false;
}

/* A long computation on this board (a sat.solve a peer sent, a local solve)
 * calls hal::compute_yield() every so often. It keeps the board a live fleet
 * member while the job runs: the watchdog petted, packets moved, the hub link
 * read and heartbeats sent. While it does, fleet_poll serves no new work - a
 * second job nested inside the first on this one thread is not a thing. */
int g_computing = 0;
void net_breadcrumb(const char* msg);   // net.cpp: a line to the LAN log (udp/42425)
extern "C" void metal_watchdog_pet();
namespace aios::hal {
void compute_yield() {
  static uint64_t last = 0;
  const uint64_t now = fleet_now_ms();
  if (now - last < 50) return;
  last = now;
  metal_watchdog_pet();
  static uint64_t yields = 0;
  if ((++yields & 63) == 1) { char b[64]; std::snprintf(b, sizeof b, "compute_yield #%llu", (unsigned long long)yields); net_breadcrumb(b); }
  ++g_computing;
  net_service(0);
  if (g_fleet_client) g_fleet_client->poll();
  --g_computing;
}
}  // namespace aios::hal

extern "C" void fleet_poll() {
  static bool busy = false;
  if (busy) return;
  if (g_computing) {
    // mid-computation: keep the link, take no new work
    if (g_fleet_client) g_fleet_client->poll();
    return;
  }
  if (!g_fleet_client) {
    busy = true;
    fleet_autojoin_step();
    busy = false;
    return;
  }
  busy = true;
  g_fleet_client->poll();
  while (g_fleet_node) {
    auto c = g_fleet_client->accept_incoming();
    if (!c) break;
    if (!g_fleet_node->adopt_polled(std::shared_ptr<aios::hal::Connection>(std::move(c))))
      kputs("  fleet: a peer's circuit failed the handshake\n");
  }
  if (g_fleet_node) g_fleet_node->poll();
  busy = false;
}

/* ---- The dashboard's Fleet tab (fleet_view.hpp) ---- */
namespace {
void copy_str(char* dst, size_t cap, const std::string& s) {
  size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
  for (size_t i = 0; i < n; ++i) dst[i] = s[i];
  dst[n] = 0;
}
}  // namespace

extern "C" void fleet_status(FleetStatus* out) {
  *out = FleetStatus{};
  size_t sz = 0;
  out->configured = fat32_status().mounted && fat32_file_size("FLEET.CFG", sz) && sz > 0;
  out->attached = g_fleet_client && g_fleet_client->attached();
  out->joining = !g_fleet_client && out->configured && !g_auto_done;
  const uint64_t now = fleet_now_ms();
  out->retry_in_s = (out->joining && g_auto_next_ms > now) ? static_cast<uint32_t>((g_auto_next_ms - now + 999) / 1000) : 0;
  if (g_fleet_client) copy_str(out->hub, sizeof(out->hub), g_fleet_client->hub_address());
  copy_str(out->self, sizeof(out->self), board_identity().short_str());
  if (g_fleet_client) {
    auto roster = g_fleet_client->roster();
    out->roster_count = static_cast<uint32_t>(roster.size());
    for (size_t i = 0; i < roster.size() && i < 16; ++i) copy_str(out->roster[i], sizeof(out->roster[i]), roster[i].short_str());
  }
  if (g_fleet_node) {
    for (auto& p : g_fleet_node->peers()) { ++out->served_count; out->served_requests += p.requests; }
  }
}

extern "C" size_t fleet_survey(FleetMember* out, size_t max) {
  if (!g_fleet_client || !g_fleet_client->attached()) return 0;
  auto roster = g_fleet_client->roster();
  /* A HARD, TOTAL cap - not just per-peer. This ran the board for the better
   * part of an hour on real hardware once already (RemoteNode::call()'s
   * 120s default before that was cut to 5s for telemetry()), and even at
   * board_reach()'s bounded 8s open() plus telemetry()'s now-5s reply, one
   * unresponsive member is still 13s, times however many are on the roster,
   * ALL on the single frame-loop core with nothing else serviced meanwhile
   * (fleet_poll() - the hub heartbeat AND every incoming fleet run/sql_on -
   * only runs from net_poll()'s outermost call, which this blocks). A crash
   * (not just a stall) followed the very next time this ran after that fix
   * landed - unrelated cause not established, but a survey that can never
   * run longer than this budget, full stop, is the conservative answer
   * until it is. Whatever did not fit gets an honest "skipped" note rather
   * than being silently left off the list. */
  constexpr uint64_t kBudgetMs = 20000;
  const uint64_t deadline = fleet_now_ms() + kBudgetMs;
  size_t n = 0;
  crashlog(("survey: start, roster=" + std::to_string(roster.size())).c_str());
  for (auto& id : roster) {
    if (n >= max) break;
    FleetMember& m = out[n++];
    m = FleetMember{};
    copy_str(m.id, sizeof(m.id), id.short_str());
    fleet_survey_progress(n - 1, roster.size(), id.short_str().c_str());
    const std::string tag = "survey[" + std::to_string(n - 1) + " " + id.short_str() + "]: ";
    if (fleet_now_ms() >= deadline) { copy_str(m.note, sizeof(m.note), "skipped - survey time budget used up"); crashlog((tag + "skipped - budget used up").c_str()); continue; }
    crashlog((tag + "board_reach() start").c_str());
    auto node = board_reach(aios_arm64::kernel(), id.str());
    crashlog((tag + "board_reach() returned").c_str());
    if (!node.ok()) { copy_str(m.note, sizeof(m.note), node.error().message); crashlog((tag + "board_reach() failed: " + node.error().message).c_str()); continue; }
    const uint64_t t0 = fleet_now_ms();
    crashlog((tag + "telemetry() start").c_str());
    auto t = (*node)->telemetry();
    crashlog((tag + "telemetry() returned").c_str());
    if (!t.ok()) { copy_str(m.note, sizeof(m.note), t.error().message); crashlog((tag + "telemetry() failed: " + t.error().message).c_str()); continue; }
    m.reached = true;
    m.rtt_ms = static_cast<uint32_t>(fleet_now_ms() - t0);
    m.cpus = t->logical_cpus; m.slots = t->slots; m.jobs = t->movable_entities;
    m.memory_in_use = t->memory_in_use; m.total_memory = t->total_memory;
    m.databases = static_cast<uint32_t>(t->hosted_databases.size());
    crashlog((tag + "ok, rtt=" + std::to_string(m.rtt_ms) + "ms").c_str());
  }
  crashlog(("survey: done, n=" + std::to_string(n)).c_str());
  return n;
}

extern "C" void fleet_request_join() {
  /* fleet_poll runs the join (it holds the reentrancy guard); this only
   * makes it due now. */
  g_auto_done = false;
  g_auto_next_ms = 0;
  g_auto_backoff_ms = 10000;
}

extern "C" void fleet_leave() {
  if (!g_fleet_client) { g_auto_done = true; return; }
  kprintf("  fleet: leaving %s (from the dashboard)\n", g_fleet_client->hub_address().c_str());
  aios::RelayClient* old = g_fleet_client;
  aios::NodeService* old_node = g_fleet_node;
  g_fleet_client = nullptr;
  g_fleet_node = nullptr;
  g_peer_links.clear();
  g_auto_done = true;                         /* stay off until someone joins again */
  old->stop();
  delete old;
  if (old_node) { old_node->stop(); delete old_node; }
}
