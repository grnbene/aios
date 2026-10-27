#include "cli.hpp"
#include "model_controller.h"
#include "grain_cmd.hpp"
#include "shell.hpp"
#include "../src/node/protocol.hpp"
#include "aios/userland/crash_log.hpp"
#include "aios/userland/runtime_registry.hpp"
#include "aios/hal/sync.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/crypto/curve25519.hpp"
// Hosted only: writing an operator status file to the host's filesystem, and
// finding a host binary beside the running executable. A bare-metal kernel has
// neither a host filesystem nor a /proc to read its own path out of — and
// dragging <fstream> in there pulls the whole locale and iconv machinery with
// it, which is exactly what a freestanding runtime cannot provide.
#include <fstream>
#include <ctime>
#include <iomanip>
#include <cstdio>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include <cctype>
#if !defined(AIOS_METAL)
#include <sys/wait.h>
#endif
#if !defined(AIOS_METAL) && defined(__linux__)
#include <sys/reboot.h>
#include <linux/reboot.h>
#endif
#if defined(AIOS_METAL)
// `reboot` and `poweroff` on the metal. Declared, not written inline: the port
// writes that used to sit in those two commands were the only x86-specific code
// in the entire core+cli tree, and so the only reason it would not compile for
// AArch64. The machine's own layer supplies the mechanism.
extern "C" [[noreturn]] void metal_power_reboot();
extern "C" [[noreturn]] void metal_power_off();
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include "aios/userland/miner.hpp"
#include "aios/userland/machine_sql.hpp"
#include "sat_cmd.hpp"
#include "aios/userland/canvas.hpp"        // userland::run_dir — the ONE run directory
#include "aios/userland/scene_description.hpp"
#include "aios/userland/key_bindings.hpp"
#include "aios/userland/web_js.hpp"
#include "aios/userland/mysql_wire.hpp"
#include "aios/userland/asql_capability.hpp"
#include "aios/sidecar/capability_guard.hpp"
#include "aios/storage/aql.hpp"
#include "aios/userland/bitcoin_wallet.hpp"
#include "aios/userland/bitcoin_contract.hpp"
#include "aios/memory/hnsw_index.hpp"
#include "aios/compute/p2p_dma.hpp"
#include "aios/compute/backends.hpp"
#include "aios/intelligence/graph_partitioner.hpp"
#include "aios/intelligence/qwen_mind.hpp"
#include "aios/intelligence/world_model.hpp"
#include "aios/intelligence/language_model.hpp"
#include "aios/channel/peer_discovery.hpp"
#include <chrono>
#include <thread>
#include "aios/channel/btc_p2p.hpp"
#include "aios/channel/chain_rpc.hpp"
#include "aios/channel/http_client.hpp"
#include "aios/channel/compute_serve.hpp"
#include "aios/channel/llm_serve.hpp"
#include "aios/userland/llm.hpp"
#include "aios/channel/gateway.hpp"
#include "aios/channel/rpc_value.hpp"
#include "aios/compute/erp_kernels.hpp"
#include "aios/userland/chain/btc_tx.hpp"
#include "aios/capability/attestation.hpp"
#include "aios/kernel/time_quota.hpp"
#include "aios/userland/compositor.hpp"
#include "aios/fleet/orchestrator.hpp"
#include "aios/hal/audio.hpp"
#include "aios/userland/daemon_mgr.hpp"
#include "aios/userland/task_ledger.hpp"
#include "aios/userland/task_list.hpp"
#include "aios/media/audio.hpp"
#include "aios/media/stt.hpp"
#include "aios/media/tts.hpp"
#include "aios/media/voice.hpp"
#include "aios/userland/git.hpp"
#include "aios/userland/git_remote.hpp"
#include "aios/userland/patch_transaction.hpp"
#include "aios/userland/npm.hpp"
#include "aios/userland/pypi.hpp"
#include "aios/userland/crates.hpp"
#include "aios/userland/gomod.hpp"
#include "aios/userland/feed_parser.hpp"
#include "aios/kernel/watchdog.hpp"
#include "aios/userland/elf.hpp"
#include "aios/userland/directive.hpp"
#include "aios/userland/horizon.hpp"
#include "aios/userland/sense.hpp"
#include "aios/userland/web.hpp"
#include "aios/userland/web_pane.hpp"
#include "aios/net/ssh.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/mail_client.hpp"
#include "aios/userland/google_sync.hpp"
#include "aios/userland/oauth.hpp"
#include "aios/core/json.hpp"
#include "aios/net/imap.hpp"
#include "aios/net/smtp.hpp"
#include "aios/channel/http_client.hpp"
#include "aios/userland/contacts_client.hpp"
#include "aios/userland/calendar_client.hpp"
#include "aios/channel/ingress_receiver.hpp"
#include "../tools/aios-git/git_cli.hpp"
#include <sstream>
#include <unistd.h>
#include "aios/core/base64.hpp"
#include "aios/userland/grain_app.hpp"
#include <algorithm>
#include <functional>

namespace aios::cli {

namespace {
// A file's lines, for the *_file options (tokens, passwords, table lists).
// Hosted only: the bare-metal build has no <fstream> (and no filesystem to
// point these at) - it reports the file unreadable, as a missing file would.
// std::ifstream inline here is what stopped metal/build.sh linking.
bool read_file_lines(const std::string& path, std::vector<std::string>& lines) {
#if !defined(AIOS_METAL)
  std::ifstream f(path);
  if (!f) return false;
  std::string l;
  while (std::getline(f, l)) {
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
    lines.push_back(l);
  }
  return true;
#else
  (void)path; (void)lines;
  return false;
#endif
}
// A cluster key given as @FILE is read from that file (first line), so the
// secret is never on a command line where ps and shell history can see it.
// env:NAME reads it from that environment variable, for the same reason.
std::string fleet_key_arg(const std::string& arg) {
  if (arg.rfind("env:", 0) == 0) {
    const char* v = std::getenv(arg.substr(4).c_str());
    if (!v || !*v) fail(Status::NotFound, "environment variable " + arg.substr(4) + " is not set (named by " + arg + ")");
    return v;
  }
  if (arg.empty() || arg[0] != '@') return arg;
  std::vector<std::string> lines;
  if (!read_file_lines(arg.substr(1), lines) || lines.empty()) fail(Status::NotFound, "cannot read the cluster key file " + arg.substr(1));
  std::string k = lines[0];
  while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
  return k;
}
// The first line of a file (a secret kept out of the unit file).
bool read_first_line(const std::string& path, std::string& out) {
  std::vector<std::string> lines;
  if (!read_file_lines(path, lines)) return false;
  out = lines.empty() ? std::string() : lines[0];
  return true;
}
}  // namespace


namespace {

// Where this machine keeps the small files a console writes and the desktop
// reads: the scene, the face, the keyboard, the url an app wants shown. Near
// the top because the commands that write into it appear all through this file.
std::string g_act_run_dir;

std::string run_dir_path() {
  const char* run_env = getenv("AOS_RUN");
  if (run_env && *run_env) return std::string(run_env);
  if (!g_act_run_dir.empty()) return g_act_run_dir;
  return std::string("run");
}

// The run directory has to EXIST before anything writes into it. It did not on
// a fresh root, and write_file writes a .tmp beside the target and renames, so
// `display show` came back with
// "io-error: <root>/run/scene.composed.tmp: No such file or directory" — the
// scene was composed correctly and then thrown away. A machine that has never
// had a run directory is exactly what a first boot is.
void ensure_run_dir(Kernel& k) {
  (void)k.host().make_dir(run_dir_path());   // host-io: the machine's own run dir (display state the desktop reads)
}

using Args = std::vector<std::string>;

template <typename T> T need(Result<T> r) { if (!r.ok()) throw Fail{r.error()}; return std::move(*r); }
void need(Result<void> r) { if (!r.ok()) throw Fail{r.error()}; }

std::string fmt_bytes(uint64_t b) {
  if (b >= (1ull << 30)) return fmt_double(b / double(1ull << 30), 6, true, 2) + " GiB";
  if (b >= (1ull << 20)) return fmt_double(b / double(1ull << 20), 6, true, 2) + " MiB";
  if (b >= 1024) return fmt_double(b / 1024.0, 6, true, 1) + " KiB";
  return fmt_u64(b) + " B";
}
// An address, written the way a machine writes one.
std::string fmt_hex(uint64_t v) {
  static const char* d = "0123456789abcdef";
  std::string s = "0x";
  bool lead = true;
  for (int i = 60; i >= 0; i -= 4) {
    uint8_t nib = (uint8_t)((v >> i) & 0xF);
    if (nib || !lead || i == 0) { s += d[nib]; lead = false; }
  }
  return s;
}

std::string fmt_ns(uint64_t ns) {
  if (ns >= 1000000000ull) return fmt_double(ns / 1e9, 6, true, 3) + " s";
  if (ns >= 1000000ull) return fmt_double(ns / 1e6, 6, true, 2) + " ms";
  if (ns >= 1000ull) return fmt_double(ns / 1e3, 6, true, 1) + " µs";
  return fmt_u64(ns) + " ns";
}
std::string printable(ByteSpan b, size_t max = 200) {
  std::string s;
  for (size_t i = 0; i < b.size() && i < max; ++i) { char c = (char)b[i]; s.push_back((c >= 32 && c < 127) ? c : (c == '\n' ? ' ' : '.')); }
  if (b.size() > max) s += "…";
  return s;
}
std::vector<std::string> split_text_lines(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  for (char ch : text) {
    if (ch == '\n') { out.push_back(cur); cur.clear(); }
    else if (ch != '\r') cur += ch;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

uint64_t to_u64(const std::string& s) { uint64_t v; if (!parse_u64(s, v)) fail(Status::Invalid, "expected a number, got '" + s + "'"); return v; }
double to_f64(const std::string& s) { double v; if (!parse_f64(s, v)) fail(Status::Invalid, "expected a number, got '" + s + "'"); return v; }
std::vector<uint64_t> parse_shape(const std::string& s) {
  std::vector<uint64_t> shape;
  for (auto& part : split(s, 'x')) shape.push_back(to_u64(part));
  if (shape.empty()) fail(Status::Invalid, "empty shape");
  return shape;
}

// Identity resolution: $var, full 32-hex id, or an unambiguous prefix over
// everything the kernel currently knows.
struct Resolver {
  Session& s;
  std::vector<Id128> universe() {
    std::vector<Id128> u;
    Kernel& k = *s.kernel;
    for (auto& e : k.entities().list()) u.push_back(e.id);
    for (auto& m : k.memory().list()) u.push_back(m.id);
    for (auto& st : k.states().list()) u.push_back(st.id);
    for (auto& c : k.channels().list()) u.push_back(c.id);
    for (auto& g : k.graphs().list()) { u.push_back(g.id); for (auto& op : g.operations) u.push_back(op.id); }
    for (auto& c : k.capabilities().list({})) u.push_back(c.id);
    for (auto& o : k.store().list()) u.push_back(o);
    for (auto* d : k.devices().all()) u.push_back(d->id());
    return u;
  }
  std::string expand(const std::string& tok) {
    if (!tok.empty() && tok[0] == '$') {
      auto it = s.vars.find(tok.substr(1));
      if (it == s.vars.end()) fail(Status::NotFound, "no variable " + tok);
      return it->second;
    }
    return tok;
  }
  Id128 id(const std::string& tok0) {
    std::string tok = expand(tok0);
    if (tok == "kernel") return s.kernel->self();
    for (auto& r : s.kernel->authority().residents()) if (r.label == tok) return r.id;   // residents by name: console, mind
    Id128 out;
    if (Id128::parse(tok, out)) return out;
    std::string low; for (char c : tok) low.push_back((c >= 'A' && c <= 'Z') ? (char)(c + 32) : c);
    std::vector<Id128> hits;
    for (auto& u : universe()) if (u.str().rfind(low, 0) == 0) { if (std::find(hits.begin(), hits.end(), u) == hits.end()) hits.push_back(u); }
    if (hits.empty()) fail(Status::NotFound, "no identity matches '" + tok + "'");
    if (hits.size() > 1) fail(Status::Invalid, "'" + tok + "' is ambiguous (" + std::to_string(hits.size()) + " identities)");
    return hits[0];
  }
  template <typename T> T as(const std::string& tok) { return T{id(tok)}; }
};

std::string kv(const Args& a, const std::string& key, const std::string& def = {}) {
  for (auto& t : a) if (t.rfind(key + "=", 0) == 0) return t.substr(key.size() + 1);
  return def;
}
bool has_kv(const Args& a, const std::string& key) { for (auto& t : a) if (t.rfind(key + "=", 0) == 0) return true; return false; }
// bind= for a listener: 127.0.0.1 (or localhost/loopback) binds loopback
// alone, 0.0.0.0 (or all, the default) every interface. Anything else is
// refused rather than silently widened: the substrate binds those two only.
bool bind_loopback_arg(const Args& a) {
  if (!has_kv(a, "bind")) return false;
  const std::string b = kv(a, "bind");
  if (b == "127.0.0.1" || b == "localhost" || b == "loopback") return true;
  if (b == "0.0.0.0" || b == "all" || b == "*") return false;
  fail(Status::Invalid, "bind=" + b + ": only bind=127.0.0.1 (loopback) or bind=0.0.0.0 (every interface) is supported");
  return false;
}

ByteBuffer parse_bytes(const std::string& t) {
  if (t.rfind("hex:", 0) == 0) {
    ByteBuffer b;
    std::string h = t.substr(4);
    if (h.size() % 2) fail(Status::Invalid, "odd hex length");
    for (size_t i = 0; i < h.size(); i += 2) { uint64_t v; if (!parse_u64("0x" + h.substr(i, 2), v)) fail(Status::Invalid, "bad hex"); b.push_back((uint8_t)v); }
    return b;
  }
  return ByteBuffer(t.begin(), t.end());
}

// The line being run, exactly as typed, for the few commands whose argument is
// source text in another language (browser eval). The tokenizer strips quotes,
// so `browser eval 'a' + "b"` reached the page as  a + b ; and run_line takes a
// trailing `=> NAME` as a binding, which is also how a JS arrow function is
// spelt.
static std::string g_raw_line;

struct Ctx {
  Session& s;
  Out& o;
  Resolver r;
  Kernel& k;      // read-only inspection (gated by view()); never authority — see scripts/authority-gate.sh
  Surface& me;    // the console entity: everything that acts, acts through here
  void produced(const std::string& id) { s.last = id; }
  // A Surface for another entity — only if the kernel lets the console act as it.
  Surface as(const std::string& tok) { return need(me.act_as(r.as<EntityID>(tok))); }
  void view() { need(me.inspect()); }
  std::string bytes_line(ByteSpan b) { return "\"" + printable(b) + "\" (" + std::to_string(b.size()) + " bytes)"; }
};

// ---------------------------------------------------------------- commands
void cmd_info(Ctx& c, const Args&) {
  c.view();
  Kernel& k = c.k;
  auto& topo = k.host().topology();
  auto ms = k.memory().stats();

  c.o << c.o.style(Header()) << "System Overview" << c.o.style(Reset()) << "\n";
  c.o << "  " << W{16} << "Kernel ID" << ": " << c.o.style(Cyan()) << k.self().str() << c.o.style(Reset()) << "\n";
  c.o << "  " << W{16} << "Console ID" << ": " << c.o.style(Cyan()) << c.me.self().str() << c.o.style(Reset()) << "  (console entity)\n";
  c.o << "  " << W{16} << "Root" << ": " << k.config().root << "\n";
  c.o << "  " << W{16} << "Uptime" << ": " << fmt_ns(k.uptime_ns()) << "\n";
  c.o << "  " << W{16} << "Host" << ": " << topo.arch << " · " << topo.host_name << " · " << topo.logical_cpus << " cpus · " << fmt_bytes(topo.total_memory) << " · " << topo.numa.size() << " NUMA node(s)\n\n";

  c.o << c.o.style(Header()) << "Substrate & Fabric Resources" << c.o.style(Reset()) << "\n";
  c.o << "  " << W{16} << "Memory Fabric" << ": " << ms.objects << " objects, " << fmt_bytes(ms.bytes)
      << " (system " << fmt_bytes(ms.bytes_by_type[0])
      << ", device " << fmt_bytes(ms.bytes_by_type[1])
      << ", shared " << fmt_bytes(ms.bytes_by_type[2])
      << ", persistent " << fmt_bytes(ms.bytes_by_type[3])
      << ", remote " << fmt_bytes(ms.bytes_by_type[4]) << "), " << ms.migrations << " migrations\n";
  c.o << "  " << W{16} << "Entities" << ": " << k.entities().count() << " active (states: " << k.states().count()
      << ", channels: " << k.channels().list().size() << ", graphs: " << k.graphs().list().size()
      << ", capabilities: " << k.capabilities().count() << ", store objects: " << k.store().count() << ")\n";

  c.o << "  " << W{16} << "Git" << ": " << userland::git_registry_line(k) << "\n";
  // What the machine is watching, and how much of the world has landed on its
  // horizon. Read-only: asking must never mint a register.
  {
    userland::Horizon hz(c.me);
    std::string line;
    if (hz.attach(false).ok()) {
      auto sig = hz.signals(1000);
      line = std::to_string(hz.sources().size()) + " source(s) watched, " + std::to_string(sig.size()) +
             " signal(s) on the horizon";
    } else {
      line = "watching nothing — `horizon watch <name> <url>` gives it somewhere to look";
    }
    c.o << "  " << W{16} << "Horizon" << ": " << line << "\n";
  }
  // What this machine can reach with: its own ssh identity, and the hosts it
  // has already met under a key it will not let change quietly.
  {
    std::string line = "no identity yet — `ssh keygen` makes one";
    auto id = net::ssh_load_identity(k.host(), k.config().root + "/ssh/id_ed25519", false);
    if (id.ok()) {
      auto known = net::ssh_known_hosts(k.host(), k.config().root + "/ssh/known_hosts");
      line = id->fingerprint() + " · " + fmt_u64(known.ok() ? known->size() : 0) + " host(s) known";
    } else if (id.status() != Status::NotFound) {
      line = "unreadable: " + id.error().str();
    }
    c.o << "  " << W{16} << "SSH" << ": " << line << "\n";
  }
  c.o << "  " << W{16} << "Devices" << ": ";
  for (auto* d : k.devices().all()) {
    c.o << c.o.style(Bold()) << d->name() << c.o.style(Reset()) << "(" << d->id().short_str() << ", " << d->capacity().lanes << " lanes";
    auto mem = d->memory_stats();
    if (mem.ok() && mem->total_bytes > 0) {
      // What kind of memory it is, by what the device is: a peer's CPU reports
      // its host RAM (native_memory is Remote for every peer device), not VRAM.
      const DeviceType dt = d->type();
      const bool host_ram = d->capacity().native_memory == MemoryType::System || dt == DeviceType::CPU || dt == DeviceType::RemoteCPU;
      const char* tag = host_ram ? (dt == DeviceType::RemoteCPU ? "remote ram" : "ram") : (dt == DeviceType::RemoteGPU || dt == DeviceType::RemoteNPU ? "remote vram" : "vram");
      c.o << ", " << fmt_bytes(mem->used_bytes) << "/" << fmt_bytes(mem->total_bytes) << " " << tag;
    }
    c.o << ") ";
  }
  c.o << "\n";
  // Name them. "1 via OpenCL" said nothing about which runtime found what, and
  // said nothing at all about the CUDA path, which is now also here.
  {
    const auto& acc = k.accelerators();
    c.o << "  " << W{16} << "Accelerators" << ": ";
    if (acc.devices.empty()) c.o << (acc.note.empty() ? "none" : acc.note) << "\n";
    else {
      for (size_t i = 0; i < acc.devices.size(); ++i) c.o << (i ? ", " : "") << acc.devices[i];
      if (!acc.note.empty()) c.o << "  (" << acc.note << ")";
      c.o << "\n";
    }
  }
}

// Storage and network are resources the machine HAS, which is a different
// question from what it has already allocated. The kernel's root is named so
// the volume persistence depends on comes back marked.
void cmd_storage(Ctx& c, const Args&) {
  c.view();
  auto vols = c.k.host().storage(c.k.config().root);
  if (!vols.ok()) fail(vols.error().status, vols.error().str());
  c.o << c.o.style(Header())
      << W{22} << "VOLUME"
      << W{12} << "KIND"
      << W{12} << "CAPACITY"
      << W{12} << "USED"
      << W{8}  << "BLOCK"
      << "PATH" << c.o.style(Reset()) << "\n";
  for (const auto& v : *vols) {
    c.o << W{22} << (v.name.size() > 21 ? v.name.substr(v.name.size() - 21) : v.name)
        << W{12} << v.kind
        << W{12} << fmt_bytes(v.capacity)
        << W{12} << fmt_bytes(v.used)
        << W{8}  << v.block_size
        << v.path << (v.holds_root ? "  ← the kernel's root" : "") << "\n";
  }
  if (vols->empty()) c.o << "  (this machine reports no storage)\n";
}

void cmd_network(Ctx& c, const Args&) {
  c.view();
  auto ifaces = c.k.host().network();
  if (!ifaces.ok()) fail(ifaces.error().status, ifaces.error().str());
  c.o << c.o.style(Header())
      << W{14} << "INTERFACE"
      << W{18} << "ADDRESS"
      << W{20} << "HARDWARE"
      << W{8}  << "MTU"
      << W{10} << "STATE"
      << "NOTE" << c.o.style(Reset()) << "\n";
  for (const auto& n : *ifaces) {
    std::string note;
    if (n.carries_fabric) note = "carries the AOS fabric";
    else if (n.loopback) note = "loopback";
    if (n.speed_bps) note += (note.empty() ? "" : " · ") + std::to_string(n.speed_bps / 1000000ull) + " Mbit/s";
    c.o << W{14} << n.name
        << W{18} << (n.address.empty() ? "-" : n.address)
        << W{20} << (n.hardware.empty() ? "-" : n.hardware)
        << W{8}  << n.mtu
        << W{10} << (n.up ? "up" : "down")
        << note << "\n";
  }
  if (ifaces->empty()) c.o << "  (this machine reports no network interfaces)\n";
}

void cmd_devices(Ctx& c, const Args&) {
  c.view();
  c.o << c.o.style(Header())
      << W{14} << "DEVICE ID"
      << W{10} << "TYPE"
      << W{8}  << "LANES"
      << W{8}  << "BUSY"
      << W{14} << "MEMORY"
      << W{10} << "NATIVE"
      << "NAME" << c.o.style(Reset()) << "\n";
  for (auto* d : c.k.devices().all()) {
    c.o << W{14} << d->id().short_str()
        << W{10} << device_type_name(d->type())
        << W{8}  << d->capacity().lanes
        << W{8}  << d->busy_lanes()
        << W{14} << fmt_bytes(d->capacity().memory)
        << W{10} << memory_type_name(d->capacity().native_memory)
        << d->name() << "\n";
    // What is on it right now. A capacity says what the card was built with;
    // this says what is left, where the number came from, and who holds the
    // rest — the training run on ordinal 1 is a tenant, not free space.
    auto mem = d->memory_stats();
    if (mem.ok() && mem->total_bytes && !mem->source.empty()) {
      c.o << W{14} << "" << "  in use " << fmt_bytes(mem->used_bytes) << ", free " << fmt_bytes(mem->free_bytes)
          << " (" << mem->source << ")";
      if (!mem->tenants.empty()) {
        c.o << "; tenants:";
        for (auto& t : mem->tenants) c.o << " pid " << t.pid << (t.name.empty() ? "" : " " + t.name) << " " << fmt_bytes(t.bytes);
      }
      c.o << "\n";
    }
  }
}

void show_entity(Ctx& c, const Entity& e);

void cmd_sysstat(Ctx& c, const Args&) {
  c.view();
  Kernel& k = c.k;
  auto& topo = k.host().topology();
  auto ms = k.memory().stats();

  c.o << c.o.style(Header()) << "AIOS System Statistics & Substrate Health" << c.o.style(Reset()) << "\n\n";

  c.o << c.o.style(Bold()) << "Substrate Identifiers & Host Topology" << c.o.style(Reset()) << "\n";
  c.o << "  " << W{18} << "Kernel Identity" << ": " << c.o.style(Cyan()) << k.self().str() << c.o.style(Reset()) << "\n";
  c.o << "  " << W{18} << "Console Identity" << ": " << c.o.style(Cyan()) << c.me.self().str() << c.o.style(Reset()) << "\n";
  c.o << "  " << W{18} << "Persistent Root" << ": " << k.config().root << "\n";
  c.o << "  " << W{18} << "Kernel Uptime" << ": " << fmt_ns(k.uptime_ns()) << "\n";
  c.o << "  " << W{18} << "Hardware Host" << ": " << topo.arch << " · " << topo.host_name << " · " << topo.logical_cpus << " cpus · " << fmt_bytes(topo.total_memory) << " · " << topo.numa.size() << " NUMA node(s)\n\n";

  // What the machine HAS, before what it has used. This view is the one a
  // reader reaches for first, and it used to show only allocation — which is
  // how a machine with a GPU and half a terabyte of disk could be read as
  // having neither. `devices`, `storage` and `network` give the full tables.
  c.o << c.o.style(Bold()) << "Machine Resources" << c.o.style(Reset()) << "\n";
  {
    std::string compute;
    for (auto* d : k.devices().all()) {
      if (!compute.empty()) compute += " · ";
      compute += d->name() + "(" + std::to_string(d->capacity().lanes) + " lanes)";
    }
    c.o << "  " << W{18} << "Compute" << ": " << (compute.empty() ? "none" : compute) << "\n";
    auto vols = k.host().storage(k.config().root);
    if (vols.ok() && !vols->empty()) {
      const auto& v = (*vols)[0];
      c.o << "  " << W{18} << "Storage" << ": " << v.name << " " << fmt_bytes(v.capacity)
          << " (" << fmt_bytes(v.used) << " used)" << (v.holds_root ? " ← holds the root" : "")
          << " · " << vols->size() << " volume(s)\n";
    } else {
      c.o << "  " << W{18} << "Storage" << ": " << (vols.ok() ? "none reported" : vols.error().str()) << "\n";
    }
    auto ifaces = k.host().network();
    if (ifaces.ok()) {
      std::string addressed;
      size_t up = 0;
      for (const auto& n : *ifaces) {
        if (n.up) ++up;
        if (n.loopback || n.address.empty()) continue;
        if (!addressed.empty()) addressed += " · ";
        addressed += n.name + " " + n.address;
      }
      if (addressed.empty() && !ifaces->empty()) for (const auto& n : *ifaces) if (n.carries_fabric) addressed = n.name + " " + n.hardware + " (fabric)";
      c.o << "  " << W{18} << "Network" << ": " << (addressed.empty() ? "no addressed interface" : addressed)
          << " · " << up << " of " << ifaces->size() << " up\n";
    } else {
      c.o << "  " << W{18} << "Network" << ": " << ifaces.error().str() << "\n";
    }
  }
  c.o << "\n";

  c.o << c.o.style(Bold()) << "Entity & Substrate Resource Table" << c.o.style(Reset()) << "\n";
  c.o << c.o.style(Header())
      << W{16} << "RESOURCE"
      << W{12} << "ACTIVE"
      << W{14} << "TOTAL BYTES"
      << "DETAILS" << c.o.style(Reset()) << "\n";
  
  c.o << W{16} << "Entities" << W{12} << k.entities().count() << W{14} << "-" << (std::to_string(k.authority().residents().size()) + " resident(s)") << "\n";
  c.o << W{16} << "States" << W{12} << k.states().count() << W{14} << "-" << "Entity context & execution state" << "\n";
  c.o << W{16} << "Memory Fabric" << W{12} << ms.objects << W{14} << fmt_bytes(ms.bytes) << (std::to_string(ms.migrations) + " migrations") << "\n";
  c.o << W{16} << "Channels" << W{12} << k.channels().list().size() << W{14} << "-" << "Ring buffer state channels" << "\n";
  c.o << W{16} << "Compute Graphs" << W{12} << k.graphs().list().size() << W{14} << "-" << "Execution waves & operations" << "\n";
  c.o << W{16} << "Capabilities" << W{12} << k.capabilities().count() << W{14} << "-" << "Derived & held authority" << "\n";
  c.o << W{16} << "Object Store" << W{12} << k.store().count() << W{14} << "-" << "Semantic persistent objects" << "\n\n";

  c.o << c.o.style(Bold()) << "Memory Fabric Provider Allocation" << c.o.style(Reset()) << "\n";
  c.o << c.o.style(Header())
      << W{14} << "PROVIDER"
      << W{16} << "ALLOCATED"
      << "DESCRIPTION" << c.o.style(Reset()) << "\n";
  c.o << W{14} << "System RAM" << W{16} << fmt_bytes(ms.bytes_by_type[0]) << "Private kernel mapped RAM" << "\n";
  c.o << W{14} << "Device" << W{16} << fmt_bytes(ms.bytes_by_type[1]) << "GPU / NPU accelerator memory" << "\n";
  c.o << W{14} << "Shared" << W{16} << fmt_bytes(ms.bytes_by_type[2]) << "memfd channel ring regions" << "\n";
  c.o << W{14} << "Persistent" << W{16} << fmt_bytes(ms.bytes_by_type[3]) << "AOS volume disk-backed memory" << "\n";
  c.o << W{14} << "Remote" << W{16} << fmt_bytes(ms.bytes_by_type[4]) << "Fabric peer node memory" << "\n\n";

  size_t refusals = 0;
  for (auto& e : k.authority().events()) if (e.kind == AuthorityEvent::Refused) refusals++;
  c.o << c.o.style(Bold()) << "Authority & Security Audit Ledger" << c.o.style(Reset()) << "\n";
  c.o << "  " << W{18} << "Total Audit Events" << ": " << k.authority().events().size() << "\n";
  c.o << "  " << W{18} << "Policy Refusals" << ": " << (refusals > 0 ? c.o.style(Red()) : c.o.style(Green())) << refusals << c.o.style(Reset()) << " (hard security boundary checks)\n";
}

// One service manager per kernel, for the life of this process. Unit
// DEFINITIONS live in the object store and outlast the process; running
// processes do not, and the manager kills what it started when it goes, because
// a supervisor that exits leaving its children behind is how a machine ends up
// with two of everything.
static userland::DaemonManager& daemons_for(Kernel& k) {
  static std::unordered_map<Kernel*, userland::DaemonManager> m;
  auto it = m.find(&k);
  if (it == m.end()) {
    it = m.emplace(std::piecewise_construct, std::forward_as_tuple(&k), std::forward_as_tuple(k.host())).first;
    // Units the machine already knows about: newest definition per name wins,
    // and a name whose newest record is a tombstone stays forgotten.
    std::map<std::string, StoredObject> newest;
    for (const auto& id : k.store().find_meta("kind", "service")) {
      auto o = k.store().get(id);
      if (!o.ok()) continue;
      std::string name = o->meta("service");
      if (name.empty()) continue;
      auto prev = newest.find(name);
      if (prev == newest.end() || prev->second.timestamp_ns <= o->timestamp_ns) newest[name] = *o;
    }
    for (const auto& [name, o] : newest) {
      if (o.meta("forgotten") == "yes") continue;
      userland::ServiceUnit u;
      u.name = name;
      u.command = std::string(o.payload.begin(), o.payload.end());
      u.working_dir = o.meta("dir");
      u.output_path = o.meta("log");
      (void)userland::parse_restart_policy(o.meta("restart"), u.restart);
      u.at_boot = o.meta("atboot") == "yes";
      for (const auto& d : split(o.meta("after"), ',')) if (!d.empty()) u.dependencies.push_back(d);
      // meta() gives one value per key, and a unit may carry several of each
      // of these, so they are read off the record directly.
      for (const auto& [mk, mv] : o.metadata) {
        if (mk == "env") {
          const size_t eq = mv.find('=');
          if (eq != std::string::npos && eq != 0) u.environment.emplace_back(mv.substr(0, eq), mv.substr(eq + 1));
        } else if (mk == "unset" && !mv.empty()) {
          u.unset_environment.push_back(mv);
        }
      }
      (void)it->second.register_unit(u);
    }
  }
  return it->second;
}

static void daemon_report(Ctx& c, const std::vector<userland::SupervisionEvent>& events) {
  for (const auto& e : events) {
    c.o << "  " << e.service << " " << e.what;
    if (e.signal) c.o << " on signal " << e.signal;
    else if (e.what == "exited") c.o << " with status " << e.exit_code;
    c.o << "\n";
  }
}

size_t start_boot_services_impl(Session& s) {
  if (!s.kernel) return 0;
  auto& mgr = daemons_for(*s.kernel);
  size_t started = 0;
  for (const auto& u : mgr.list_services()) {
    if (!u.at_boot) continue;
    if (u.status == userland::ServiceStatus::Running) continue;
    auto r = mgr.start_service(u.name);
    if (r.ok()) {
      ++started;
      std::fprintf(stderr, "service %s started with the machine\n", u.name.c_str());
    } else {
      std::fprintf(stderr, "service %s would not start: %s\n", u.name.c_str(),
                   r.error().message.c_str());
    }
  }
  return started;
}

size_t supervise_services_impl(Session& s) {
  if (!s.kernel) return 0;
  auto events = daemons_for(*s.kernel).supervise();
  // stderr, not the console's Out: this runs on a clock, not inside a command,
  // and a supervision event appearing halfway through somebody's output would
  // read as part of the answer. On the desktop stderr is the machine's log,
  // which is where you go to find out why a service is not there any more.
  for (const auto& e : events) {
    if (e.signal)
      std::fprintf(stderr, "service %s %s on signal %d\n", e.service.c_str(), e.what.c_str(), e.signal);
    else if (e.what == "exited")
      std::fprintf(stderr, "service %s exited with status %d\n", e.service.c_str(), e.exit_code);
    else
      std::fprintf(stderr, "service %s %s\n", e.service.c_str(), e.what.c_str());
  }
  return events.size();
}

// The machine's keyboard. It had none of this: every key was a branch, so the
// machine could not say what a chord did, let alone change it — which is what
// happens when you ask it to re-do its bindings and it has nothing to operate.
// Asking the substrate to go down, and finding out whether it agreed.
//
// This used to run a chain of shell commands, DISCARD the result, try the
// reboot syscall (which needs privileges this process does not have), and then
// std::exit(0) regardless. Under a supervisor with Restart=always that last
// line is the bug: the request was refused, the process exited, and the
// desktop came straight back — which reads exactly like the machine rebooting
// itself when you asked it to power off. It also said so only in a log file.
//
// Returns empty on success (the machine is going down and nothing here will
// run again), or the reason it refused.
std::string ask_substrate_for_power(bool off) {
  const char* verb = off ? "poweroff" : "reboot";
  // systemd first, then the same through the machine's own escalation, then
  // the older tools. A machine with passwordless sudo can do this; one without
  // is entitled to be told so rather than watching its desktop bounce.
  const std::string attempts[] = {
    std::string("systemctl ") + verb + " 2>&1",
    std::string("sudo -n systemctl ") + verb + " 2>&1",
    std::string(off ? "sudo -n /sbin/poweroff" : "sudo -n /sbin/reboot") + " 2>&1",
    std::string(off ? "shutdown -h now" : "shutdown -r now") + " 2>&1",
  };
  std::string last;
  for (const auto& cmd : attempts) {
    std::string out;
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) continue;
    char buf[256];
    while (std::fgets(buf, sizeof buf, pipe)) out += buf;
    const int rc = ::pclose(pipe);
    if (rc == 0) return std::string();   // accepted; the machine is going down
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (!out.empty()) last = out;
  }
  if (last.empty()) last = "the substrate refused and said nothing";
  return last;
}

void cmd_keys(Ctx& c, const Args& a) {
  const std::string usage =
      "keys | keys actions | keys bind <chord> <action> | keys unbind <chord> | keys reset";
  const std::string path = userland::key_bindings_path(c.k.config().root);

  // Whatever is on disk, over the machine's defaults — the same way the
  // compositor loads it, so what this prints is what the display obeys.
  ensure_run_dir(c.k);
  userland::KeyBindings binds = userland::KeyBindings::defaults();
  {
    auto f = c.k.host().read_file(path);   // host-io: the machine's key-binding table (userland::key_bindings_path)
    if (f.ok() && f->size() > 0) {
      (void)binds.apply_text(std::string((const char*)f->data(), f->size()));
    }
  }

  const std::string sub = a.size() > 1 ? a[1] : "list";

  if (sub == "list" || sub == "status") {
    c.view();
    // The action column is as wide as the widest slug plus a gap; it was a
    // fixed 18, and "desktop.screenshot" is 18, so it ran into its own text.
    int act_w = 8;
    for (uint8_t i = 0; i < (uint8_t)userland::UiAction::Count; ++i)
      act_w = std::max(act_w, (int)std::strlen(userland::action_slug((userland::UiAction)i)) + 2);
    c.o << c.o.style(Header()) << W{14} << "KEY" << W{act_w} << "ACTION" << "WHAT IT DOES"
        << c.o.style(Reset()) << "\n";
    for (const auto& b : binds.all()) {
      const std::string name = userland::chord_name(b.key);
      if (name.empty()) continue;
      c.o << W{14} << name << W{act_w} << userland::action_slug(b.action)
          << userland::action_help(b.action) << "\n";
    }
    c.o << "\nkept in " << path << ", which the compositor re-reads twice a second\n";
    c.o << "`keys actions` lists every action a key can be given.\n";
    return;
  }

  if (sub == "actions") {
    c.view();
    int act_w = 8;
    for (uint8_t i = 0; i < (uint8_t)userland::UiAction::Count; ++i)
      act_w = std::max(act_w, (int)std::strlen(userland::action_slug((userland::UiAction)i)) + 2);
    c.o << c.o.style(Header()) << W{act_w} << "ACTION" << "WHAT IT DOES" << c.o.style(Reset()) << "\n";
    for (uint8_t i = 0; i < (uint8_t)userland::UiAction::Count; ++i) {
      const auto act = (userland::UiAction)i;
      c.o << W{act_w} << userland::action_slug(act) << userland::action_help(act) << "\n";
    }
    return;
  }

  if (sub == "bind") {
    if (a.size() < 4) fail(Status::Invalid, "keys bind <chord> <action>   (chords: F1, Ctrl+R, Escape, Space, or one character)");
    need(c.me.operate());
    int key = 0;
    if (!userland::parse_chord(a[2], key))
      fail(Status::Invalid, "no key called '" + a[2] + "' - try F1, Ctrl+R, Escape, Space, or one character");
    userland::UiAction act;
    if (!userland::parse_action(a[3], act))
      fail(Status::Invalid, "no action called '" + a[3] + "' - `keys actions` lists them");
    binds.bind(key, act);
    const std::string text = binds.to_text();
    need(c.k.host().write_file(path, ByteSpan((const uint8_t*)text.data(), text.size())));   // host-io: the machine's key-binding table (userland::key_bindings_path)
    c.o << userland::chord_name(key) << " now does " << userland::action_slug(act)
        << " - " << userland::action_help(act) << "\n";
    c.o << "  written to " << path << "; the display picks it up within half a second\n";
    return;
  }

  if (sub == "unbind") {
    if (a.size() < 3) fail(Status::Invalid, "keys unbind <chord>");
    need(c.me.operate());
    int key = 0;
    if (!userland::parse_chord(a[2], key)) fail(Status::Invalid, "no key called '" + a[2] + "'");
    const bool had = binds.unbind(key);
    const std::string text = binds.to_text();
    need(c.k.host().write_file(path, ByteSpan((const uint8_t*)text.data(), text.size())));   // host-io: the machine's key-binding table (userland::key_bindings_path)
    c.o << userland::chord_name(key) << (had ? " does nothing now\n" : " was not bound anyway\n");
    return;
  }

  if (sub == "reset" || sub == "default" || sub == "defaults") {
    need(c.me.operate());
    userland::KeyBindings fresh = userland::KeyBindings::defaults();
    const std::string text = fresh.to_text();
    need(c.k.host().write_file(path, ByteSpan((const uint8_t*)text.data(), text.size())));   // host-io: the machine's key-binding table (userland::key_bindings_path)
    c.o << "the keyboard is back to this machine's defaults (" << fresh.all().size() << " bindings)\n";
    return;
  }

  fail(Status::Invalid, usage);
}

void cmd_daemon(Ctx& c, const Args& a) {
  const std::string usage =
      "daemon register <name> \"<command>\" [after=a,b] [restart=never|on-failure|always] [dir=<path>] [log=<path>] "
      "[env=K=V ...] [unset=NAME,NAME] [atboot] | "
      "daemon list | start <name> | stop <name> | restart <name> | status <name> | supervise [seconds] | forget <name>";
  if (a.size() < 2) fail(Status::Invalid, usage);
  const std::string& sub = a[1];
  auto& mgr = daemons_for(c.k);

  if (sub == "list") {
    c.view();
    (void)mgr.supervise();
    c.o << c.o.style(Header())
        << W{16} << "SERVICE" << W{12} << "STATUS" << W{12} << "RESTART"
        << W{10} << "RESTARTS" << W{12} << "UPTIME" << W{10} << "LAST EXIT" << "COMMAND"
        << c.o.style(Reset()) << "\n";
    uint64_t now = c.k.host().monotonic_ns();
    for (const auto& u : mgr.list_services()) {
      std::string exit_note = u.last_signal ? ("signal " + std::to_string(u.last_signal))
                            : (u.stopped_ns ? std::to_string(u.last_exit_code) : std::string("-"));
      c.o << W{16} << u.name
          << W{12} << userland::service_status_name(u.status)
          << W{12} << userland::restart_policy_name(u.restart)
          << W{10} << u.restart_count
          << W{12} << (u.status == userland::ServiceStatus::Running && u.started_ns ? fmt_ns(now - u.started_ns) : std::string("-"))
          << W{10} << exit_note
          << u.command << "\n";
      if (!u.dependencies.empty()) {
        c.o << "                  after:";
        for (const auto& d : u.dependencies) c.o << " " << d;
        c.o << "\n";
      }
      if (!u.last_error.empty()) c.o << "                  " << c.o.style(Red()) << u.last_error << c.o.style(Reset()) << "\n";
    }
  } else if (sub == "register" || sub == "define") {
    if (a.size() < 4) fail(Status::Invalid, usage);
    need(c.me.operate());
    userland::ServiceUnit u;
    u.name = a[2];
    u.command = c.r.expand(a[3]);
    for (size_t i = 4; i < a.size(); ++i) {
      const std::string& t = a[i];
      if (t.rfind("after=", 0) == 0) { for (auto& d : split(t.substr(6), ',')) if (!d.empty()) u.dependencies.push_back(d); }
      else if (t.rfind("restart=", 0) == 0) { if (!userland::parse_restart_policy(t.substr(8), u.restart)) fail(Status::Invalid, "restart= never|on-failure|always"); }
      else if (t.rfind("dir=", 0) == 0) u.working_dir = c.r.expand(t.substr(4));
      else if (t.rfind("log=", 0) == 0) u.output_path = c.r.expand(t.substr(4));
      // env= may be repeated. A service that needs a port, a state directory
      // or a token cannot be described without this, and the unit had no way
      // to say it: ServiceUnit has carried an environment since it was written
      // and only the command surface could not fill it in.
      else if (t.rfind("env=", 0) == 0) {
        const std::string kv = c.r.expand(t.substr(4));
        const size_t eq = kv.find('=');
        if (eq == std::string::npos || eq == 0) fail(Status::Invalid, "env=NAME=VALUE");
        u.environment.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
      }
      else if (t == "atboot" || t == "atboot=yes" || t == "atboot=true") u.at_boot = true;
      else if (t == "atboot=no" || t == "atboot=false") u.at_boot = false;
      else if (t.rfind("unset=", 0) == 0) {
        for (auto& n : split(c.r.expand(t.substr(6)), ',')) if (!n.empty()) u.unset_environment.push_back(n);
      }
      else if (t.rfind("limit=", 0) == 0) u.restart_limit = (uint32_t)to_u64(t.substr(6));
      else if (t.rfind("delay=", 0) == 0) u.restart_delay_ns = to_u64(t.substr(6));
      else fail(Status::Invalid, "unknown service attribute '" + t + "'");
    }
    need(mgr.register_unit(u));
    StoredObject o;
    o.payload = ByteBuffer(u.command.begin(), u.command.end());
    o.metadata.emplace_back("kind", "service");
    o.metadata.emplace_back("service", u.name);
    o.metadata.emplace_back("restart", userland::restart_policy_name(u.restart));
    if (!u.working_dir.empty()) o.metadata.emplace_back("dir", u.working_dir);
    if (!u.output_path.empty()) o.metadata.emplace_back("log", u.output_path);
    if (u.at_boot) o.metadata.emplace_back("atboot", "yes");
    for (const auto& [ek, ev] : u.environment) o.metadata.emplace_back("env", ek + "=" + ev);
    for (const auto& n : u.unset_environment) o.metadata.emplace_back("unset", n);
    if (!u.dependencies.empty()) {
      std::string after;
      for (const auto& d : u.dependencies) after += (after.empty() ? "" : ",") + d;
      o.metadata.emplace_back("after", after);
    }
    ObjectID id = need(c.me.store(std::move(o)));
    c.o << "service " << u.name << " defined (object " << id.short_str() << "): " << u.command << "\n";
  } else if (sub == "start") {
    if (a.size() < 3) fail(Status::Invalid, "daemon start <name>");
    need(c.me.operate());
    need(mgr.start_service(a[2]));
    auto u = need(mgr.get_service(a[2]));
    c.o << "service " << u.name << " " << userland::service_status_name(u.status) << "\n";
  } else if (sub == "stop") {
    if (a.size() < 3) fail(Status::Invalid, "daemon stop <name>");
    need(c.me.operate());
    need(mgr.stop_service(a[2]));
    c.o << "service " << a[2] << " stopped\n";
  } else if (sub == "restart") {
    if (a.size() < 3) fail(Status::Invalid, "daemon restart <name>");
    need(c.me.operate());
    need(mgr.restart_service(a[2]));
    c.o << "service " << a[2] << " restarted\n";
  } else if (sub == "status") {
    if (a.size() < 3) fail(Status::Invalid, "daemon status <name>");
    c.view();
    (void)mgr.supervise();
    auto u = need(mgr.get_service(a[2]));
    uint64_t now = c.k.host().monotonic_ns();
    c.o << "  " << W{18} << "Service" << ": " << u.name << "\n";
    c.o << "  " << W{18} << "Command" << ": " << u.command << "\n";
    c.o << "  " << W{18} << "Status" << ": " << userland::service_status_name(u.status) << "\n";
    c.o << "  " << W{18} << "Restart policy" << ": " << userland::restart_policy_name(u.restart)
        << " (limit " << u.restart_limit << " per " << fmt_ns(u.restart_window_ns) << ")\n";
    c.o << "  " << W{18} << "Restarts" << ": " << u.restart_count << "\n";
    if (u.status == userland::ServiceStatus::Running) c.o << "  " << W{18} << "Uptime" << ": " << fmt_ns(now - u.started_ns) << "\n";
    if (u.stopped_ns) c.o << "  " << W{18} << "Last exit" << ": " << (u.last_signal ? "signal " + std::to_string(u.last_signal) : "status " + std::to_string(u.last_exit_code)) << "\n";
    if (!u.output_path.empty()) c.o << "  " << W{18} << "Output" << ": " << u.output_path << "\n";
    // What the child will actually be handed. A service whose environment
    // cannot be read back is a service nobody can debug: the difference
    // between the port you meant and the port it got is invisible otherwise.
    c.o << "  " << W{18} << "At boot" << ": " << (u.at_boot ? "yes - started with the machine" : "no") << "\n";
    for (const auto& [ek, ev] : u.environment) c.o << "  " << W{18} << "Env" << ": " << ek << "=" << ev << "\n";
    if (!u.unset_environment.empty()) {
      c.o << "  " << W{18} << "Stripped" << ":";
      for (const auto& n : u.unset_environment) c.o << " " << n;
      c.o << "\n";
    }
    if (!u.dependencies.empty()) { c.o << "  " << W{18} << "After" << ":"; for (auto& d : u.dependencies) c.o << " " << d; c.o << "\n"; }
    if (!u.last_error.empty()) c.o << "  " << W{18} << "Note" << ": " << u.last_error << "\n";
  } else if (sub == "supervise") {
    double seconds = a.size() > 2 ? to_f64(a[2]) : 0.0;
    uint64_t deadline = c.k.host().monotonic_ns() + (uint64_t)(seconds * 1e9);
    c.o << "supervising " << mgr.list_services().size() << " service(s) for " << seconds << "s\n";
    c.o.flush();
    for (;;) {
      auto events = mgr.supervise();
      if (!events.empty()) { daemon_report(c, events); c.o.flush(); }
      if (c.k.host().monotonic_ns() >= deadline) break;
      hal::sleep_ms(100);
    }
    c.o << "supervision pass complete\n";
  } else if (sub == "forget") {
    if (a.size() < 3) fail(Status::Invalid, "daemon forget <name>");
    need(c.me.operate());
    (void)mgr.stop_service(a[2]);
    need(mgr.forget_service(a[2]));
    StoredObject o;
    o.metadata.emplace_back("kind", "service");
    o.metadata.emplace_back("service", a[2]);
    o.metadata.emplace_back("forgotten", "yes");
    (void)c.me.store(std::move(o));
    c.o << "service " << a[2] << " forgotten\n";
  } else fail(Status::Invalid, usage);
}

// Applications. An application on this machine is a SERVICE plus two things a
// service does not have: a name a person would use, and an interface served
// over http that this kernel's own web engine can draw. So this builds on the
// daemon manager rather than beside it — one supervisor, one restart policy,
// one place a process is reaped.
//
// A dream is an app too — its record carries the same kind=app, with a file
// url into the kernel root — so the launcher lists what the machine dreamed
// beside what was installed, and `app open` shows either.
struct AppPackage {
  const char* name;
  const char* what;
  const char* command;
  const char* url;
  const char* dir;
};
// Packaged with the machine. `app install <name>` materialises one; nothing is
// registered until somebody asks, so a built-in never fights a service the
// operator already defined under the same name. The command is found on PATH
// and an empty dir keeps the daemon's own, so no one host's layout is baked in.
const AppPackage kPackaged[] = {
  {"assay", "the ASSAY harness and its web interface",
   "assay serve", "http://127.0.0.1:4318/", ""},
};

std::string app_record_url(Kernel& k, const std::string& name) {
  std::string newest_url;
  uint64_t newest = 0;
  for (const auto& id : k.store().find_meta("kind", "app")) {
    auto o = k.store().get(id);
    if (!o.ok() || o->meta("app") != name) continue;
    if (o->timestamp_ns >= newest) { newest = o->timestamp_ns; newest_url = o->meta("url"); }
  }
  if (!newest_url.empty() && newest_url != "removed") return newest_url;

  // Check store dreams
  for (const auto& id : k.store().find_meta("kind", "dream")) {
    auto o = k.store().get(id);
    if (!o.ok() || o->meta("dream") != name) continue;
    if (o->timestamp_ns >= newest) { newest = o->timestamp_ns; newest_url = o->meta("url"); }
  }
  if (!newest_url.empty()) return newest_url;

  // Check packaged apps
  for (const auto& p : kPackaged) {
    if (name == p.name) return p.url;
  }

  // Check dreams on disk: <root>/dreams/<name>/face.aos
  if (k.host().exists(k.config().root + "/dreams/" + name + "/face.aos")) {
    return "face://" + name;
  }
  return {};
}

std::vector<std::pair<std::string, std::string>> app_records(Kernel& k) {
  std::map<std::string, std::pair<uint64_t, std::string>> newest;
  for (const auto& id : k.store().find_meta("kind", "app")) {
    auto o = k.store().get(id);
    if (!o.ok()) continue;
    const std::string n = o->meta("app");
    if (n.empty()) continue;
    auto it = newest.find(n);
    if (it == newest.end() || it->second.first <= o->timestamp_ns) {
      newest[n] = {o->timestamp_ns, o->meta("url")};
    }
  }
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto& [n, v] : newest) if (v.second != "removed") out.emplace_back(n, v.second);
  return out;
}

// This machine's JavaScript, without a page. For proving the evaluator and for
// answering "what does this engine actually support" with a list rather than
// folklore.
void cmd_js(Ctx& c, const Args& a) {
  if (a.size() < 2) fail(Status::Invalid, "js <expression or program> | js features");
  if (a[1] == "features" || a[1] == "support") {
    c.view();
    c.o << userland::js_supported_features();
    return;
  }
  std::string src;
  if (a[1] == "run" || a[1] == "file") {
    // From a file, because the console's own tokeniser takes the quotes and
    // backticks out of an argument and a script is mostly quotes.
    if (a.size() < 3) fail(Status::Invalid, "js run <path>");
    auto f = need(c.k.host().read_file(c.r.expand(a[2])));   // host-io: a file the operator named on the command line
    src.assign((const char*)f.data(), f.size());
  } else {
    for (size_t i = 1; i < a.size(); ++i) { if (i > 1) src += " "; src += a[i]; }
  }
  std::string err;
  const std::string out = userland::js_eval_to_text(src, err);
  if (!err.empty()) fail(Status::Invalid, err);
  c.o << out;
  if (!out.empty() && out.back() != '\n') c.o << "\n";
}

// ---- grain apps (aios/userland/grain_app.hpp) ----
//
// The same apps grain hosts on its own, hosted by this kernel (`serve`) and kept under <root>/apps in grain's own
// layout: a host's directory moves between a grain and an aios unchanged, and either's `app` command manages
// either's host. What aios adds is the fleet: `app ... on <member>` / `on ALL` is carried by the node link.

std::atomic<bool> g_serve_stop{false};   // SIGTERM / SIGINT end `serve`, its apps first
void serve_stop_signal(int) { g_serve_stop.store(true); }

std::string apps_home(Kernel& k) { return k.config().root + "/apps"; }

std::vector<std::string> aios_app_caps() { return {"grain", "http", "db", "files", "process", "aios", "sql", "fleet"}; }

userland::grain::apps::AppCommandEnv aios_app_env(Kernel& k) {
  userland::grain::apps::AppCommandEnv env;
  env.home = apps_home(k);
  env.setup_vm = [&k](userland::grain::Vm& vm) { setup_app_vm(k, vm); };
  env.capabilities = aios_app_caps();
  return env;
}

// What a grain app command answers, here: its output and exit status.
int run_app_line(Kernel& k, const std::vector<std::string>& args, std::string& out) {
  std::ostringstream o, e;
  const int rc = userland::grain::apps::run_app_command(k.host(), args, aios_app_env(k), o, e);
  out = o.str() + e.str();
  return rc;
}

// The words that make `app` a grain-app command (the rest are the packaged services `app` has always managed).
bool grain_app_word(const std::string& w) {
  static const char* words[] = {"deploy", "status", "logs", "rollback", "versions", "restart", "set", "unset", "secret",
                                "settings", "secrets", "check", "pack", "run", "hosts", "add-host", "help"};
  for (const char* x : words) if (w == x) return true;
  return false;
}

bool is_grain_app(Kernel& k, const std::string& name) {
  return !name.empty() && k.host().exists(apps_home(k) + "/apps/" + name + "/record.json");
}

// `app ...`: on this machine's host, a host named in <root>/apps/hosts.json, or fleet members (`on <member>|ALL`).
void grain_app_command(Ctx& c, const Args& a) {
  std::vector<std::string> args(a.begin() + 1, a.end());
  for (auto& x : args) x = c.r.expand(x);
  // `on X` where X is the fleet (ALL) or one of its members: the node link carries the command there
  std::string on;
  size_t on_at = args.size();
  for (size_t i = 0; i + 1 < args.size(); ++i) if (args[i] == "on") { on = args[i + 1]; on_at = i; }
  std::vector<Id128> peers;
  if (!on.empty() && c.k.in_fleet()) {
    for (auto& id : c.k.fleet_roster())
      if (on == "ALL" || on == "all" || id.str().compare(0, on.size(), on) == 0) peers.push_back(id);
  }
  if (peers.empty() && (on == "ALL" || on == "all")) on.clear();   // ALL without a fleet: this machine
  if (peers.empty()) {
    if (on.empty() && on_at < args.size()) args.erase(args.begin() + (long)on_at, args.begin() + (long)on_at + 2);
    std::string out;
    const int rc = run_app_line(c.k, args, out);
    c.o << out;
    if (rc != 0) fail(Status::IoError, "app " + (args.empty() ? std::string() : args[0]) + " failed");
    return;
  }
  args.erase(args.begin() + (long)on_at, args.begin() + (long)on_at + 2);
  // A deploy is packed here, where its directory is, and sent whole.
  if (!args.empty() && args[0] == "deploy" && args.size() > 1 && args[1].rfind("@bundle:", 0) != 0) {
    auto b = need(userland::grain::apps::pack_dir(c.k.host(), args[1]));
    need(userland::grain::apps::compile_all(b));
    const std::string enc = b.encode();
    args[1] = "@bundle:" + aios::base64_encode(ByteSpan(reinterpret_cast<const uint8_t*>(enc.data()), enc.size()));
  }
  std::string line = "app";
  for (const auto& x : args) line += " " + x;
  bool any_failed = false;
  if ((on == "ALL" || on == "all") && c.k.host().exists(apps_home(c.k) + "/host.key")) {   // this machine too, if it hosts apps
    std::string out;
    c.o << "== " << c.k.self().short_str() << " (this machine)\n";
    if (run_app_line(c.k, args, out) != 0) any_failed = true;
    c.o << out;
  }
  for (const auto& id : peers) {
    c.o << "== " << id.short_str() << "\n";
    auto at = c.k.reach(id);
    for (int tries = 0; !at.ok() && tries < 5; ++tries) { hal::sleep_ms(1000); at = c.k.reach(id); }   // a member just (re)started
    if (!at.ok()) { c.o << "  cannot reach it: " << at.error().message << "\n"; any_failed = true; continue; }
    auto res = c.k.nodes()[*at]->command(line);
    if (!res.ok()) { c.o << "  " << res.error().message << "\n"; any_failed = true; continue; }
    c.o << res->second;
    if (!res->second.empty() && res->second.back() != '\n') c.o << "\n";
    if (res->first != 0) any_failed = true;
  }
  if (any_failed) fail(Status::IoError, "app " + (args.empty() ? std::string() : args[0]) + " did not succeed everywhere");
}

// ---- an app route given to another machine (on_machine), run through the substrate ----------------------------
// The request is the input buffer of one Custom operation (kernel "app.route"); the scheduler places it on the member's
// remote device (device_selector "remote:relay:<member>"), the fabric stages the request there, the member's app host
// answers it in-process into the output buffer, and the answer comes back as the operation's output (trailing zeros
// left off on the link). No new protocol: a graph, as any other work aios distributes.
constexpr const char* kAppRouteKernel = "app.route";

ByteBuffer encode_app_request(const std::string& app, const userland::grain::WebUi::Request& q) {
  ByteBuffer b;
  Writer w(b);
  w.str(app); w.str(q.method); w.str(q.path); w.str(q.query); w.str(q.cookie);
  w.u8((uint8_t)((q.xhr ? 1 : 0) | (q.https ? 2 : 0) | (q.tree ? 4 : 0) | (q.stream ? 8 : 0)));   // an older member reads bits 1, 2
  w.str(q.body); w.str(q.remote);
  w.u32((uint32_t)q.headers.size());
  for (const auto& h : q.headers) { w.str(h.first); w.str(h.second); }
  return b;
}
bool decode_app_request(ByteSpan in, std::string& app, userland::grain::WebUi::Request& q) {
  Reader r(in);
  app = r.str(); q.method = r.str(); q.path = r.str(); q.query = r.str(); q.cookie = r.str();
  const uint8_t f = r.u8();
  q.xhr = (f & 1) != 0; q.https = (f & 2) != 0; q.tree = (f & 4) != 0; q.stream = (f & 8) != 0;
  q.body = r.str(); q.remote = r.str();
  const uint32_t n = r.u32();
  for (uint32_t i = 0; i < n && r.ok(); ++i) { std::string k = r.str(); std::string v = r.str(); q.headers.push_back({k, v}); }
  return r.ok();
}
// The answer in its buffer: u32 length of what follows, then status, headers, body. A length past the buffer says
// the answer did not fit: its size, so the caller can ask again with room for it.
ByteBuffer encode_app_response(const userland::grain::WebUi::Response& res) {
  ByteBuffer b;
  Writer w(b);
  w.u32(0);
  w.u32((uint32_t)res.status);
  w.u32((uint32_t)res.headers.size());
  for (const auto& h : res.headers) { w.str(h.first); w.str(h.second); }
  w.str(res.body);
  const uint32_t len = (uint32_t)(b.size() - 4);
  for (int i = 0; i < 4; ++i) b[(size_t)i] = (uint8_t)(len >> (8 * i));
  return b;
}
bool decode_app_response(ByteSpan in, userland::grain::WebUi::Response& res) {
  Reader r(in);
  (void)r.u32();
  res.status = (int)r.u32();
  const uint32_t n = r.u32();
  for (uint32_t i = 0; i < n && r.ok(); ++i) { std::string k = r.str(); std::string v = r.str(); res.headers.push_back({k, v}); }
  res.body = r.str();
  return r.ok();
}

// serve [port=7480] [all-ips=1]: this machine's app host. Runs until stopped (SIGTERM), its apps with it.
void cmd_serve(Ctx& c, const Args& a) {
  namespace ga = userland::grain::apps;
  const std::string home = apps_home(c.k);
  auto token = need(ga::host_token(c.k.host(), home));
  auto store = ga::make_dir_store(c.k.host(), home);
  // An app route given to a fleet member (on_machine) runs there as a graph: this host's own entity builds it.
  using K = ResourceKind;
  AuthorityProfile route_profile = {
      {ResourceRef{K::Kernel, {}}, (uint16_t)R_Schedule, 0},
      {ResourceRef{K::ComputeDevice, {}}, (uint16_t)R_Execute, 0},
      {ResourceRef{K::Memory, {}}, (uint16_t)(R_Read | R_Write | R_Allocate | R_Destroy), 4ull << 30},
  };
  EntityID route_entity = need(c.me.resident("app-host", route_profile));
  Surface route_worker = need(c.me.act_as(route_entity));
  std::mutex reach_mu;   // Kernel::reach is not made for many threads at once; one request reaches a member at a time
  // At most AIOS_APP_ROUTE_MAX (default 8) forwarded requests at once: each holds an answer buffer (64 MB by default)
  // and the host has a memory cap. A request over the limit waits up to 20 s for a turn, then answers 503.
  const int route_max = [] {
    const char* e = std::getenv("AIOS_APP_ROUTE_MAX");
    const long v = e && *e ? std::strtol(e, nullptr, 10) : 8;
    return (int)std::max<long>(1, std::min<long>(v, 256));
  }();
  std::mutex slots_mu;
  std::condition_variable slots_cv;
  int slots_used = 0;
  // The first answer buffer a slot takes (AIOS_APP_ROUTE_MB, default 1); it grows to the largest answer it has had.
  const uint64_t route_room = [] {
    const char* e = std::getenv("AIOS_APP_ROUTE_MB");
    const uint64_t mb = e && *e ? std::strtoull(e, nullptr, 10) : 1;
    return std::max<uint64_t>(1, mb) << 20;
  }();
  // Each of the route_max slots keeps its request and answer buffers from one request to the next. Fresh ones a
  // request were two MemAllocs (one of a 64 MB answer buffer the member zero-filled), two MemReleases and the job
  // lane's uploads - five round trips through the hub (~50 ms each, Cleve -> chi -> atl) for an answer that took
  // the member a millisecond. Kept, the buffers stay staged on the member and a request is one ExecBatch.
  struct RouteBufs { Tensor tin{}, tout{}; uint64_t in_cap = 0, out_cap = 0; bool busy = false; };
  std::vector<RouteBufs> route_bufs((size_t)route_max);
  ga::Host::Options host_opt{[&c](userland::grain::Vm& vm) { setup_app_vm(c.k, vm); }, aios_app_caps(), c.k.self().short_str(), nullptr};
  host_opt.forward = [&c, &route_worker, &reach_mu, route_room, route_max, &slots_mu, &slots_cv, &slots_used, &route_bufs](const std::string& app, const std::string& member,
                                                                  const userland::grain::WebUi::Request& req,
                                                                  userland::grain::WebUi::Response& res) -> bool {
    auto answer = [&res](int status, const std::string& why) {
      res.status = status;
      res.headers = {{"Content-Type", "text/plain; charset=utf-8"}};
      res.body = why + "\n";
      return true;
    };
    if (c.k.self().str().compare(0, member.size(), member) == 0) return false;   // this machine is that member
    // The member: from the hub's roster, a link already made, or its direct route (AIOS_FLEET_DIRECT) - so a member
    // reached straight is served while the hub is down.
    Id128 target;
    size_t node_at = 0;
    // The member and a link to it (its devices join this machine's fabric once; a dead link is replaced). A member
    // that is restarting - its app host updated - is gone from the roster and refuses links for a second or two:
    // tried for up to 15 s, inside a caller's usual 30 s, before this answers 503.
    std::string reach_err;
    auto reach_member = [&]() -> bool {
      for (int tries = 0; tries < 60; ++tries) {
        if (tries) hal::sleep_ms(250);
        std::lock_guard<std::mutex> g(reach_mu);
        auto who = c.k.find_member(member);
        if (!who.ok()) { reach_err = c.k.in_fleet() ? who.error().message : "this route runs on " + member + ", and this machine cannot reach it: " + who.error().message; continue; }
        target = *who;
        auto at = c.k.reach(target);
        if (!at.ok()) { reach_err = "cannot reach " + member + ": " + at.error().message; continue; }
        node_at = *at;
        return true;
      }
      return false;
    };
    if (!reach_member()) return answer(503, reach_err);
    {
      std::unique_lock<std::mutex> lk(slots_mu);
      if (!slots_cv.wait_for(lk, std::chrono::seconds(20), [&] { return slots_used < route_max; }))
        return answer(503, "too many requests on their way to " + member + " (" + std::to_string(route_max) + " at once); try again");
      ++slots_used;
    }
    // A slot of its own: its buffers, kept from the last request through it.
    RouteBufs* rb = nullptr;
    {
      std::lock_guard<std::mutex> g(slots_mu);
      for (auto& b : route_bufs) if (!b.busy) { b.busy = true; rb = &b; break; }
    }
    struct Slot {
      std::mutex& m; std::condition_variable& cv; int& used; RouteBufs* rb;
      ~Slot() { { std::lock_guard<std::mutex> g(m); --used; if (rb) rb->busy = false; } cv.notify_one(); }
    } slot{slots_mu, slots_cv, slots_used, rb};
    if (!rb) return answer(503, "no free route slot");
    // A buffer that cannot be used as it is goes, and the next request through the slot makes a new one.
    auto drop = [&](Tensor& t, uint64_t& cap) { if (cap) { (void)route_worker.release(t.memory); cap = 0; } };
    auto ensure = [&](Tensor& t, uint64_t& cap, uint64_t need) -> bool {
      if (cap >= need) return true;
      drop(t, cap);
      auto nt = route_worker.tensor(DType::U8, {need});
      if (!nt.ok()) return false;
      t = *nt;
      cap = need;
      return true;
    };
    const ByteBuffer in = encode_app_request(app, req);
    // The request buffer: room for this request (a longer one grows it); what is past it the decoder never reads.
    if (!ensure(rb->tin, rb->in_cap, std::max<uint64_t>(in.size(), 4096))) return answer(503, "request buffer");
    // The member restarted (its app host was updated, say): the link this machine held to it is dead, and so are the
    // slot's buffers, which lived in its old process. The first request through it failed ("no reply from node"),
    // answered 502 - and the sticker feed's PHP front took a 502 as "grain is down" and fell back to its own feed,
    // minutes long. Such a failure now closes the dead link, reaches the member again, makes the buffers anew and
    // asks once more; an app's own failure is answered as it is.
    bool relinked = false;
    auto link_gone = [](const std::string& why) {
      return why.find("closed the link") != std::string::npos || why.find("no reply from node") != std::string::npos ||
             why.find("is not in the fabric") != std::string::npos;
    };
    auto relink = [&]() -> bool {
      relinked = true;
      drop(rb->tout, rb->out_cap);
      drop(rb->tin, rb->in_cap);
      {
        std::lock_guard<std::mutex> g(reach_mu);
        if (node_at < c.k.nodes().size() && c.k.nodes()[node_at]->kernel() == target) c.k.nodes()[node_at]->close();
      }
      return reach_member() && ensure(rb->tin, rb->in_cap, std::max<uint64_t>(in.size(), 4096));
    };
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (!ensure(rb->tout, rb->out_cap, std::max<uint64_t>(rb->out_cap, route_room))) return answer(503, "answer buffer");
      auto wr = route_worker.write(rb->tin.memory, rb->tin.offset, ByteSpan(in.data(), in.size()));
      if (!wr.ok() && !relinked && link_gone(wr.error().message)) {
        if (!relink()) return answer(503, reach_err.empty() ? "cannot reach " + member + " again after its link closed" : reach_err);
        --attempt;
        continue;
      }
      auto g = route_worker.create_graph();
      if (!wr.ok() || !g.ok()) return answer(503, "graph: " + (wr.ok() ? g.error().message : wr.error().message));
      Operation op;
      op.code = OpCode::Custom;
      op.custom_kernel = fnv1a(std::string(kAppRouteKernel));
      op.requirement.device_selector = "remote:relay:" + target.short_str();   // the member's link, direct or through the hub
      Tensor tin = rb->tin;
      tin.shape = {(uint64_t)in.size()};   // the request's own bytes, not the buffer's room
      op.inputs.push_back(tin);
      op.outputs.push_back(rb->tout);
      auto added = route_worker.add_operation(*g, op);
      auto rep = added.ok() ? route_worker.execute(*g) : Result<ExecutionReport>(added.error());
      (void)route_worker.destroy_graph(*g);
      if (!rep.ok() || !rep->failure.empty()) {
        const std::string why = rep.ok() ? rep->failure : rep.error().message;
        if (!relinked && link_gone(why)) {
          if (!relink()) return answer(503, reach_err.empty() ? "cannot reach " + member + " again after its link closed" : reach_err);
          --attempt;
          continue;
        }
        drop(rb->tout, rb->out_cap);   // a failed job may have left the member's copy of it half written
        return answer(502, "on " + member + ": " + why);
      }
      auto head = route_worker.read(rb->tout.memory, rb->tout.offset, 4);
      if (!head.ok() || head->size() < 4) return answer(502, "on " + member + ": no answer came back");
      const uint64_t len = (uint64_t)(*head)[0] | ((uint64_t)(*head)[1] << 8) | ((uint64_t)(*head)[2] << 16) | ((uint64_t)(*head)[3] << 24);
      if (len + 4 > rb->out_cap) {   // it did not fit: room for it (and a quarter more), asked once more
        const uint64_t want = len + 4 + (len + 4) / 4;
        drop(rb->tout, rb->out_cap);
        if (!ensure(rb->tout, rb->out_cap, want)) return answer(503, "answer buffer");
        continue;
      }
      auto body = route_worker.read(rb->tout.memory, rb->tout.offset, len + 4);
      if (!body.ok() || !decode_app_response(ByteSpan(body->data(), body->size()), res))
        return answer(502, "on " + member + ": the answer could not be read");
      return true;
    }
    return answer(502, "on " + member + ": the answer kept outgrowing its buffer");
  };
  // A pane given to a member (ui.pane "machine") is answered here when that member is this machine.
  host_opt.is_here = [&c](const std::string& member) { return !member.empty() && c.k.self().str().compare(0, member.size(), member) == 0; };
  // A route's stream (server-sent events) from a member: one AppStream on a link of its own, each piece of the
  // member's route written through as it comes. The member's app answers it (the stream runner below, there).
  host_opt.forward_stream = [&c, &reach_mu](const std::string& app, const std::string& member, const userland::grain::WebUi::Request& req,
                                            userland::grain::StreamHeaderCallback on_headers, userland::grain::StreamChunkCallback on_chunk) -> bool {
    if (c.k.self().str().compare(0, member.size(), member) == 0) return false;   // this machine is that member
    auto fail = [&](int status, const std::string& why) {
      on_headers(status, {{"Content-Type", "text/plain; charset=utf-8"}});
      on_chunk(why + "\n");
      return true;
    };
    std::shared_ptr<RemoteNode> link;
    {
      std::lock_guard<std::mutex> g(reach_mu);
      auto who = c.k.find_member(member);
      if (!who.ok()) return fail(503, who.error().message);
      auto at = c.k.reach(*who);
      if (!at.ok()) return fail(503, "cannot reach " + member + ": " + at.error().message);
      auto sib = c.k.nodes()[*at]->open_sibling();   // a stream holds its link: the shared one stays free
      if (!sib.ok()) return fail(503, "cannot open a stream to " + member + ": " + sib.error().message);
      link = *sib;
    }
    const ByteBuffer in = encode_app_request(app, req);
    bool headed = false;
    auto r = link->stream(nodeproto::AppStream, ByteSpan(in.data(), in.size()), [&](ByteSpan f) -> bool {
      Reader rd(f);
      const uint8_t kind = rd.u8();
      if (kind == 1) {
        const int status = (int)rd.u32();
        const uint32_t n = rd.u32();
        std::vector<std::pair<std::string, std::string>> hs;
        for (uint32_t i = 0; i < n && rd.ok(); ++i) { std::string k = rd.str(); std::string v = rd.str(); hs.push_back({k, v}); }
        headed = true;
        return on_headers(status, hs);
      }
      if (kind == 2) return on_chunk(rd.str());
      return true;
    }, 90000);   // the route sends at least a comment every 25 s
    link->close();
    if (!r.ok() && !headed) return fail(502, "on " + member + ": " + r.error().message);
    return true;
  };
  ga::Host apps(c.k.host(), *store, host_opt);
  // What a member's route given to this machine runs: the request answered by the app here, in-process.
  for (auto* d : c.k.devices().of_type(DeviceType::CPU)) {
    need(d->register_kernel(kAppRouteKernel, [&apps](KernelArgs& ka) -> Result<void> {
      if (ka.op.inputs.empty() || ka.op.outputs.empty()) return Result<void>(Status::Invalid, "app.route takes a request and an answer buffer");
      const Tensor& tin = ka.op.inputs[0];
      const Tensor& tout = ka.op.outputs[0];
      std::string app;
      userland::grain::WebUi::Request req;
      if (!decode_app_request(ByteSpan(ka.inputs[0].base + tin.offset, tin.bytes()), app, req))
        return Result<void>(Status::Corrupt, "app.route: a malformed request");
      auto res = apps.answer_forwarded(app, req);
      if (!res.ok()) return res.error();
      const ByteBuffer out = encode_app_response(*res);
      uint8_t* dst = ka.outputs[0].base + tout.offset;
      const uint64_t room = tout.bytes();
      if (out.size() > room) {   // only the size: the caller asks again with room for it
        std::memcpy(dst, out.data(), 4);
        return Ok();
      }
      std::memcpy(dst, out.data(), out.size());
      // The caller keeps this buffer for its next request: what an earlier, longer answer left past this one is
      // zeroed, so the read back (which leaves trailing zeros off) carries this answer only.
      std::memset(dst + out.size(), 0, room - out.size());
      return Ok();
    }));
  }
  ga::ControlServer control(c.k.host(), apps, token);
  const uint16_t port = has_kv(a, "port") ? (uint16_t)to_u64(kv(a, "port")) : 7480;
  const bool all_ips = has_kv(a, "all-ips") && kv(a, "all-ips") != "0";
  auto bound = need(control.listen(port, !all_ips));
  const std::string ps = std::to_string(bound) + "\n";
  (void)c.k.host().write_file(home + "/host.port", ByteSpan(reinterpret_cast<const uint8_t*>(ps.data()), ps.size()));   // host-io: the app host's port, beside its key, for `app` on this machine to find it
  apps.start_all();
  // Fleet members ask this host about its apps over the node link; any other line is what it was before.
  auto runner = [&c](const std::string& line, std::string& out) -> int {
    if (line.rfind("app ", 0) == 0 || line == "app") {
      auto words = userland::split_command(line);
      return run_app_line(c.k, std::vector<std::string>(words.begin() + 1, words.end()), out);
    }
    return fleet_shell_command(line, out);   // anything but `app ...`: refused unless AIOS_FLEET_SHELL=1
  };
  // The node service takes the fleet's circuits (Kernel::fleet_join's acceptor) and answers commands and work alike.
  // The relay's own command server is only for a kernel with no service: running both, each circuit went to
  // whichever took it first, and one the command server took offered no devices - work given there (an app.route)
  // found "no device answering" in the caller's fabric.
  if (c.k.service()) c.k.service()->set_command_runner(runner);
  // A member's stream request (AppStream) for one of this host's apps: its route's writes sent back as frames.
  if (c.k.service())
    c.k.service()->set_stream_runner([&apps](ByteSpan request, const std::function<bool(ByteSpan)>& emit) -> Result<void> {
      std::string app;
      userland::grain::WebUi::Request req;
      if (!decode_app_request(request, app, req)) return Result<void>(Status::Corrupt, "a malformed app stream request");
      auto head = [&emit](int status, const std::vector<std::pair<std::string, std::string>>& hs) -> bool {
        ByteBuffer b;
        Writer w(b);
        w.u8(1);
        w.u32((uint32_t)status);
        w.u32((uint32_t)hs.size());
        for (const auto& h : hs) { w.str(h.first); w.str(h.second); }
        return emit(ByteSpan(b.data(), b.size()));
      };
      auto chunk = [&emit](const std::string& s) -> bool {
        ByteBuffer b;
        Writer w(b);
        w.u8(2);
        w.str(s);
        return emit(ByteSpan(b.data(), b.size()));
      };
      return apps.answer_forwarded_stream(app, req, head, chunk);
    });
  else if (c.k.relay()) c.k.relay()->set_command_runner(runner);
  // (straight out: this command does not return, and what it says belongs in the service's log now)
  userland::runtime::announce("app host", "aios serve", home);   // grain ps (the Host keeps its apps in the record)
  std::cout << "serve: " << store->apps().size() << " app(s) in " << home << "; control on " << (all_ips ? "all IPs" : "127.0.0.1") << " port " << bound
            << (c.k.in_fleet() ? "; fleet members reach it as `app ... on " + c.k.self().short_str() + "`" : std::string()) << "\n" << std::flush;
  g_serve_stop.store(false);
  std::signal(SIGTERM, serve_stop_signal);
  std::signal(SIGINT, serve_stop_signal);
  while (!g_serve_stop.load()) hal::sleep_ms(200);
  apps.shutdown();
  control.close();
  std::cout << "serve: stopped\n" << std::flush;
  std::_Exit(0);
}

void cmd_app(Ctx& c, const Args& a) {
  {   // grain apps first: their words, or start/stop/remove of an app this machine's host has
    const std::string w = a.size() > 1 ? a[1] : "";
    const bool on_fleet = std::find(a.begin(), a.end(), std::string("on")) != a.end();
    if (grain_app_word(w) || ((w == "start" || w == "stop" || w == "remove") && a.size() > 2 && (is_grain_app(c.k, a[2]) || on_fleet))) {
      grain_app_command(c, a);
      return;
    }
  }
  const std::string usage =
      "app list | app install <name> | app add <name> \"<command>\" url=<url> [dir=<path>] [env=K=V] [atboot] | "
      "app start <name> | app stop <name> | app open <name> | app remove <name>";
  const std::string sub = a.size() > 1 ? a[1] : "list";
  auto& mgr = daemons_for(c.k);

  auto state_of = [&](const std::string& name) -> std::string {
    auto u = mgr.get_service(name);
    if (!u.ok()) return "not installed";
    return userland::service_status_name(u->status);
  };

  if (sub == "list" || sub == "status") {
    c.view();
    (void)mgr.supervise();
    const auto installed = app_records(c.k);
    c.o << c.o.style(Header()) << W{14} << "APP" << W{14} << "STATE" << W{34} << "INTERFACE"
        << "COMMAND" << c.o.style(Reset()) << "\n";
    for (const auto& [name, url] : installed) {
      auto u = mgr.get_service(name);
      c.o << W{14} << name << W{14} << state_of(name) << W{34} << (url.empty() ? "-" : url)
          << (u.ok() ? u->command : std::string("-")) << "\n";
    }
    if (installed.empty()) c.o << "  nothing installed yet\n";
    c.o << "\n" << c.o.style(Header()) << "PACKAGED WITH THIS MACHINE" << c.o.style(Reset()) << "\n";
    for (const auto& p : kPackaged) {
      bool have = false;
      for (const auto& [n, u] : installed) if (n == p.name) have = true;
      c.o << "  " << W{12} << p.name << (have ? "installed" : "available — `app install " + std::string(p.name) + "`")
          << "\n      " << p.what << "\n";
    }
    c.o << "\n`app open <name>` puts its interface on this machine's own web pane; its scripts run\n";
    c.o << "in this kernel's own engine (`js features` says exactly what that engine speaks).\n";
    return;
  }

  if (sub == "install") {
    if (a.size() < 3) fail(Status::Invalid, "app install <name>   (`app list` names what is packaged)");
    need(c.me.operate());
    const AppPackage* pkg = nullptr;
    for (const auto& p : kPackaged) if (a[2] == p.name) pkg = &p;
    if (!pkg) fail(Status::NotFound, "nothing packaged under '" + a[2] + "'");

    // Only define the service if there is not one already: the operator may
    // have registered it by hand with an environment this package cannot know.
    if (!mgr.get_service(pkg->name).ok()) {
      userland::ServiceUnit u;
      u.name = pkg->name;
      u.command = pkg->command;
      u.argv = userland::split_command(u.command);
      u.working_dir = pkg->dir;
      u.output_path = run_dir_path() + "/" + pkg->name + ".log";
      u.restart = userland::RestartPolicy::Always;
      need(mgr.register_unit(u));
      StoredObject so;
      so.payload = ByteBuffer(u.command.begin(), u.command.end());
      so.metadata.emplace_back("kind", "service");
      so.metadata.emplace_back("service", u.name);
      so.metadata.emplace_back("restart", userland::restart_policy_name(u.restart));
      so.metadata.emplace_back("dir", u.working_dir);
      so.metadata.emplace_back("log", u.output_path);
      (void)c.me.store(std::move(so));
      c.o << "service " << pkg->name << " defined from the package\n";
    } else {
      c.o << "service " << pkg->name << " already defined — keeping it as it is\n";
    }
    StoredObject o;
    o.metadata.emplace_back("kind", "app");
    o.metadata.emplace_back("app", pkg->name);
    o.metadata.emplace_back("url", pkg->url);
    ObjectID id = need(c.me.store(std::move(o)));
    c.o << "app " << pkg->name << " installed (object " << id.short_str() << "): " << pkg->what << "\n";
    c.o << "  interface " << pkg->url << " — `app open " << pkg->name << "`\n";
    return;
  }

  if (sub == "add") {
    if (a.size() < 4) fail(Status::Invalid, usage);
    need(c.me.operate());
    userland::ServiceUnit u;
    u.name = a[2];
    u.command = c.r.expand(a[3]);
    u.argv = userland::split_command(u.command);
    u.restart = userland::RestartPolicy::Always;
    u.output_path = run_dir_path() + "/" + u.name + ".log";
    std::string url;
    for (size_t i = 4; i < a.size(); ++i) {
      const std::string& t = a[i];
      if (t.rfind("url=", 0) == 0) url = c.r.expand(t.substr(4));
      else if (t.rfind("dir=", 0) == 0) u.working_dir = c.r.expand(t.substr(4));
      else if (t.rfind("log=", 0) == 0) u.output_path = c.r.expand(t.substr(4));
      else if (t == "atboot") u.at_boot = true;
      else if (t.rfind("env=", 0) == 0) {
        const std::string kv2 = c.r.expand(t.substr(4));
        const size_t eq = kv2.find('=');
        if (eq == std::string::npos || eq == 0) fail(Status::Invalid, "env=NAME=VALUE");
        u.environment.emplace_back(kv2.substr(0, eq), kv2.substr(eq + 1));
      } else fail(Status::Invalid, "unknown app attribute '" + t + "'");
    }
    need(mgr.register_unit(u));
    StoredObject so;
    so.payload = ByteBuffer(u.command.begin(), u.command.end());
    so.metadata.emplace_back("kind", "service");
    so.metadata.emplace_back("service", u.name);
    so.metadata.emplace_back("restart", userland::restart_policy_name(u.restart));
    if (!u.working_dir.empty()) so.metadata.emplace_back("dir", u.working_dir);
    so.metadata.emplace_back("log", u.output_path);
    if (u.at_boot) so.metadata.emplace_back("atboot", "yes");
    for (const auto& [ek, ev] : u.environment) so.metadata.emplace_back("env", ek + "=" + ev);
    (void)c.me.store(std::move(so));
    StoredObject o;
    o.metadata.emplace_back("kind", "app");
    o.metadata.emplace_back("app", u.name);
    o.metadata.emplace_back("url", url);
    need(c.me.store(std::move(o)));
    c.o << "app " << u.name << " added: " << u.command << "\n";
    if (!url.empty()) c.o << "  interface " << url << "\n";
    return;
  }

  if (sub == "start" || sub == "stop") {
    if (a.size() < 3) fail(Status::Invalid, "app " + sub + " <name>");
    need(c.me.operate());
    if (sub == "start") need(mgr.start_service(a[2]));
    else need(mgr.stop_service(a[2]));
    c.o << "app " << a[2] << " " << state_of(a[2]) << "\n";
    return;
  }

  if (sub == "remove" || sub == "uninstall") {
    if (a.size() < 3) fail(Status::Invalid, "app remove <name>");
    need(c.me.operate());
    (void)mgr.stop_service(a[2]);
    (void)mgr.forget_service(a[2]);
    StoredObject o;
    o.metadata.emplace_back("kind", "app");
    o.metadata.emplace_back("app", a[2]);
    o.metadata.emplace_back("url", "removed");
    (void)c.me.store(std::move(o));
    StoredObject t;
    t.metadata.emplace_back("kind", "service");
    t.metadata.emplace_back("service", a[2]);
    t.metadata.emplace_back("forgotten", "yes");
    (void)c.me.store(std::move(t));
    c.o << "app " << a[2] << " removed\n";
    return;
  }

  if (sub == "open" || sub == "show") {
    if (a.size() < 3) fail(Status::Invalid, "app open <name>");
    need(c.me.operate());
    const std::string url = app_record_url(c.k, a[2]);
    if (url.empty()) fail(Status::NotFound, "app '" + a[2] + "' has no interface url - `app add ... url=<url>`");

    // Start it if it is not up. Opening a pane on a dead port shows an error
    // page and blames the network for a service nobody started.
    auto u = mgr.get_service(a[2]);
    if (u.ok() && u->status != userland::ServiceStatus::Running) {
      auto started = mgr.start_service(a[2]);
      if (!started.ok()) c.o << "  could not start it: " << started.error().message << "\n";
      else c.o << "  started " << a[2] << "\n";
    }
    // Wait for it to answer if it is an HTTP service, rather than assuming.
    // Native face, file, organ, and dream apps do not use HTTP and open immediately.
    bool answered = false;
    const bool local_file = url.compare(0, 7, "file://") == 0;
    const bool is_face = userland::is_face_url(url);
    const bool is_http = (url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0);

    if (local_file) {
      answered = c.k.host().exists(url.substr(7));
    } else if (is_face) {
      const std::string path = userland::face_source_path(c.k.config().root, url);
      answered = path.empty() || c.k.host().exists(path);
    } else if (!is_http) {
      answered = true;
    } else {
      for (int i = 0; i < 20 && !answered; ++i) {
        auto probe = channel::http_get(url, 1500);
        if (probe.ok() && probe->status > 0) answered = true;
        else hal::sleep_ms(500);
      }
    }
    c.o << (answered ? (is_face ? "  its face is at " : (local_file ? "  its surface is at " : "  it is answering on ")) + url + "\n"
                     : "  it is not answering on " + url + " yet; opening anyway\n");

    ensure_run_dir(c.k);
    // The url AND a serial. The desktop opens this file when its contents
    // CHANGE, so asking twice for the same application wrote the same bytes and
    // the second click did nothing at all -- which is exactly what a person
    // does when the first one seems not to have worked.
    const std::string request = url + "\n" + std::to_string(c.k.host().monotonic_ns() / 1000000ULL);
    need(c.k.host().write_file(run_dir_path() + "/browse",   // host-io: the machine's own run dir (display state the desktop reads)
                               ByteSpan((const uint8_t*)request.data(), request.size())));
    c.o << "app " << a[2] << " is on this machine's web pane (" << url << ")\n";
    c.o << "  the desktop picks it up within half a second; nothing was restarted\n";
    return;
  }

  fail(Status::Invalid, usage);
}

// Every kv entry for `key` this entity holds, newest first. Two processes on
// one root that set the same key at once each create one; the newest wins.
static std::vector<const StateObject*> kv_entries(Ctx& c, const std::string& key, const std::vector<StateObject>& all) {
  std::vector<const StateObject*> out;
  const std::string prefix = "kv:" + key + "=";
  for (auto& st : all) {
    std::string s(st.payload.begin(), st.payload.end());
    if (s.rfind(prefix, 0) == 0) out.push_back(&st);
  }
  std::sort(out.begin(), out.end(), [](const StateObject* x, const StateObject* y) { return x->updated_ns > y->updated_ns; });
  (void)c;
  return out;
}

void cmd_kv(Ctx& c, const Args& a) {
  if (a.size() < 2) fail(Status::Invalid, "kv set|get|del|delete|remove|list|clear");
  const std::string& sub = a[1];
  if (sub == "set") {
    if (a.size() < 3) fail(Status::Invalid, "kv set <key> [value]");
    std::string key = a[2];
    std::string val;
    for (size_t i = 3; i < a.size(); ++i) {
      if (i > 3) val += " ";
      val += a[i];
    }
    std::string payload_str = "kv:" + key + "=" + val;
    ByteBuffer p(payload_str.begin(), payload_str.end());

    StateID target_id;
    bool found = false;
    {
      auto all = c.k.states().list(c.me.self());
      auto entries = kv_entries(c, key, all);
      if (!entries.empty()) { target_id = entries[0]->id; found = true; }
      for (size_t i = 1; i < entries.size(); ++i) need(c.me.drop_state(entries[i]->id));   // fold duplicates
    }
    // A kv entry is kept in the root, so the next process on this root sees it.
    auto keep = [&](const StateID& sid) {
      auto kept = c.me.keep_state(sid);
      if (!kept.ok()) c.o << "  (not kept across boots: " << kept.error().message << ")\n";
    };
    if (found) {
      need(c.me.set_state(target_id, p));
      c.produced(target_id.str());
      c.o << "kv updated: " << key << " = " << val << " (state " << target_id.short_str() << ")\n";
      keep(target_id);
    } else {
      auto id = need(c.me.create_state(StateKind::Working, p));
      c.produced(id.str());
      c.o << "kv set: " << key << " = " << val << " (state " << id.short_str() << ")\n";
      keep(id);
    }
    c.s.vars["last"] = val;
  } else if (sub == "get") {
    if (a.size() < 3) fail(Status::Invalid, "kv get <key>");
    std::string key = a[2];
    auto all = c.k.states().list(c.me.self());
    auto entries = kv_entries(c, key, all);
    if (entries.empty()) fail(Status::NotFound, "key not found: '" + key + "'");
    std::string s(entries[0]->payload.begin(), entries[0]->payload.end());
    std::string val = s.substr(("kv:" + key + "=").size());
    c.s.vars["read"] = val;
    c.o << val << "\n";
  } else if (sub == "del" || sub == "delete" || sub == "remove" || sub == "rm") {
    if (a.size() < 3) fail(Status::Invalid, "kv del <key>");
    std::string key = a[2];
    auto all = c.k.states().list(c.me.self());
    auto entries = kv_entries(c, key, all);
    if (entries.empty()) fail(Status::NotFound, "key not found: '" + key + "'");
    for (auto* e : entries) need(c.me.drop_state(e->id));   // the state and its file in the root
    c.o << "deleted key '" << key << "'\n";
  } else if (sub == "list" || sub == "ls") {
    c.view();
    std::vector<std::pair<std::string, const StateObject*>> pairs;
    auto states = c.k.states().list(c.me.self());
    for (auto& st : states) {
      std::string s(st.payload.begin(), st.payload.end());
      if (s.rfind("kv:", 0) == 0 && s.size() > 3) {
        size_t eq = s.find('=', 3);
        if (eq != std::string::npos) {
          std::string key = s.substr(3, eq - 3);
          // one row per key: the newest entry, as get reads it
          auto it = std::find_if(pairs.begin(), pairs.end(), [&](const auto& pr) { return pr.first == key; });
          if (it == pairs.end()) pairs.push_back({key, &st});
          else if (st.updated_ns > it->second->updated_ns) it->second = &st;
        }
      }
    }
    c.o << c.o.style(Header())
        << W{16} << "KEY"
        << W{24} << "VALUE"
        << W{16} << "STATE ID"
        << W{6}  << "GEN" << c.o.style(Reset()) << "\n";
    for (auto& [k, st_ptr] : pairs) {
      std::string s(st_ptr->payload.begin(), st_ptr->payload.end());
      std::string val = s.substr(s.find('=', 3) + 1);
      c.o << W{16} << k
          << W{24} << val
          << W{16} << st_ptr->id.short_str()
          << W{6}  << st_ptr->generation << "\n";
    }
    if (pairs.empty()) c.o << "(no kv entries stored)\n";
  } else if (sub == "clear") {
    c.view();
    size_t count = 0;
    std::vector<StateID> to_destroy;
    for (auto& st : c.k.states().list(c.me.self())) {
      std::string s(st.payload.begin(), st.payload.end());
      if (s.rfind("kv:", 0) == 0 && s.size() > 3) {
        to_destroy.push_back(st.id);
      }
    }
    ByteBuffer empty_bytes;
    for (auto& id : to_destroy) {
      if (c.me.set_state(id, empty_bytes).ok()) count++;
    }
    c.o << "cleared " << count << " kv pair(s)\n";
  } else fail(Status::Invalid, "unknown: kv " + sub);
}

// The web engine, at the command surface.
//
// There used to be a `resolve_browser_bin()` here that hunted for a compiled
// Unblink binary and handed it to the substrate's host tool to spawn. On the
// metal that host tool has no such verb, so `browser` failed with the host's
// usage string and quietly fell back to reading the page as plain text -- and
// on a hosted machine it spawned a WebKit process that never exits, so the
// command hung. Neither is a browser. The engine is in this kernel now
// (src/userland/web_*.cpp): the same layout that paints the desktop's pane is
// run here and printed, so the serial console and the glass agree about the
// page.

// One browser per session, so `browser follow <n>` and `browser back` have a
// page to act on.
static aios::hal::Host* g_console_clock_host = nullptr;
static double console_wall_ms() {
  if (!g_console_clock_host) return 1757433600000.0;
  return (double)(g_console_clock_host->wall_ns() / 1000000ULL);
}
static void install_console_clock(Kernel& k) {
  if (g_console_clock_host) return;
  g_console_clock_host = &k.host();
  userland::js_set_clock(&console_wall_ms);
}

static std::shared_ptr<userland::web::Browser>& session_browser(Ctx& c) {
  static std::shared_ptr<userland::web::Browser> b;
  if (!b) b = std::make_shared<userland::web::Browser>(userland::kernel_fetch(c.k));
  return b;
}

// Metrics for a console: one cell per character, which is what a terminal is.
// Handing these to the same layout the pane uses is why `browser` at the
// prompt wraps where the console wraps rather than where a 1200px window does.
static userland::web::Metrics console_metrics() {
  userland::web::Metrics m;
  m.measure = [](const std::string& t, const userland::web::FontKey& f) {
    // Count characters, not bytes: a UTF-8 em dash is one column.
    int n = 0;
    for (size_t i = 0; i < t.size(); ++i)
      if (((unsigned char)t[i] & 0xC0) != 0x80) ++n;
    // Bigger type still takes more room, or a heading would wrap like body
    // text and the page would lose its shape.
    return n * 8 * (f.px > 15 ? f.px : 15) / 15;
  };
  m.line_height = [](const userland::web::FontKey& f) { return f.px > 15 ? 21 * f.px / 15 : 21; };
  m.ascent = [](const userland::web::FontKey& f) { return f.px > 15 ? 16 * f.px / 15 : 16; };
  return m;
}

static void print_page(Ctx& c, userland::web::Browser& b, int columns) {
  const auto& doc = b.document();
  c.o << c.o.style(Bold()) << (doc.title.empty() ? doc.url : doc.title) << c.o.style(Reset()) << "\n";
  c.o << c.o.style(Cyan()) << doc.url << c.o.style(Reset());
  if (!b.transport().empty()) c.o << "  ·  " << b.transport();
  c.o << "  ·  " << doc.fragments.size() << " box(es), " << doc.links.size() << " link(s), "
      << doc.content_height << "px tall\n";
  // What script did, because a page that came out empty needs to say whether
  // nothing ran or nothing was there to run.
  {
    const auto& sr = b.script();
    if (sr.scripts > 0 || !sr.error.empty()) {
      c.o << "  ·  script: " << sr.scripts << " block(s), " << sr.dom_writes << " dom write(s), "
          << sr.listeners << " listener(s)";
      if (!sr.error.empty()) c.o << c.o.style(Red()) << "  ·  stopped: " << sr.error << c.o.style(Reset());
      c.o << "\n";
      for (const auto& l : sr.log) c.o << "     " << c.o.style(Dim()) << "console:" << c.o.style(Reset()) << " " << l << "\n";
    }
  }
  c.o << "\n";
  for (const auto& line : b.as_text(columns)) c.o << line << "\n";
  if (doc.links.empty()) return;
  c.o << "\n" << c.o.style(Bold()) << "links" << c.o.style(Reset()) << " (browser follow <n>)\n";
  // A link that wrapped onto two lines has two boxes and two entries; list it
  // once, under the first index that can be followed.
  size_t shown = 0;
  for (size_t i = 0; i < doc.links.size() && shown < 60; ++i) {
    bool dup = false;
    for (size_t j = 0; j < i; ++j)
      if (doc.links[j].href == doc.links[i].href) { dup = true; break; }
    if (dup) continue;
    std::string label = doc.links[i].text;
    if (label.size() > 44) label = label.substr(0, 41) + "...";
    c.o << "  " << W{5} << ("[" + std::to_string(i) + "]") << W{46} << label << doc.links[i].href << "\n";
    ++shown;
  }
}

void cmd_browser(Ctx& c, const Args& a) {
  if (a.size() < 2)
    fail(Status::Invalid, "browser <url> | browser follow <n> | browser back | browser forward | browser page");
  need(c.me.operate());

  auto& b = session_browser(c);
  // The same clock the pane gives a page, so `browser` at the prompt and the
  // pane on the screen agree about what time it is.
  install_console_clock(c.k);
  auto m = console_metrics();
  // The width the console actually is, so the page wraps where the terminal
  // does. 100 columns at 8px a column is what the layout is told.
  const int columns = 100;
  int viewport = columns * 8 + 36;
#if !defined(AIOS_METAL)
  // AIOS_BROWSER_WIDTH drives the page at the width a real window has, so what
  // this prints is what the desktop's pane lays out. Without it every drive
  // from the shell was a 836px viewport -- a phone, as far as a page's media
  // queries are concerned, and a different layout from the one on screen.
  if (const char* w = std::getenv("AIOS_BROWSER_WIDTH")) {
    const long px = std::strtol(w, nullptr, 10);
    if (px >= 200 && px <= 8000) viewport = (int)px;
  }
#endif

  std::string verb = a[1];

  // Drive a click into the page from here, so what a person does with a
  // pointer can be done without one — which is the only way to reproduce an
  // interaction bug without a display in front of you.
  // Every box the layout produced, with the element it belongs to. What a
  // pointer can hit, in a form you can read without a pointer.
  // The tree as it stands, AFTER script has had it. What the layout is walking,
  // which is the only way to tell a CSS problem from a DOM one.
  // What the page said to itself. An application logs its own state changes,
  // and when its interface comes up empty that log is the only account of why.
  // Ask the page a question in its own heap. This is how you find out why an
  // interface came up empty: call the thing that was supposed to fill it.
  if (verb == "eval" || verb == "js") {
    if (!b->document().dom) fail(Status::NotFound, "no page loaded yet - browser <url> first");
    if (!b->js()) fail(Status::NotFound, "this page has no script");
    if (a.size() < 3) fail(Status::Invalid, "browser eval <expression>");
    std::string src = a[2];
    for (size_t i = 3; i < a.size(); ++i) src += " " + a[i];
    // The expression verbatim: everything after `browser eval ` on the line
    // as typed, quotes and `=>` included.
    {
      const std::string& L = g_raw_line;
      size_t p = L.find_first_not_of(" \t");
      if (p != std::string::npos && L[p] == '!') ++p;
      auto word = [&](const std::string& w) {
        p = L.find_first_not_of(" \t", p);
        if (p == std::string::npos || L.compare(p, w.size(), w) != 0) return false;
        p += w.size();
        return p < L.size() && (L[p] == ' ' || L[p] == '\t');
      };
      if (word(a[0]) && word(verb) && L.find_first_not_of(" \t", p) != std::string::npos) {
        std::string rest = L.substr(L.find_first_not_of(" \t", p));
        while (!rest.empty() && (rest.back() == '\r' || rest.back() == '\n')) rest.pop_back();
        if (!rest.empty()) src = rest;
      }
    }
    std::string err;
    int wrote = 0;
    const std::string out = b->js()->eval(src, err, &wrote);
    if (!err.empty()) c.o << "error: " << err << "\n";
    else c.o << out << "\n";
    if (wrote > 0) {
      b->relayout(m, viewport);
      c.o << "  " << wrote << " dom change(s), " << b->document().fragments.size() << " box(es)\n";
    }
    return;
  }

  if (verb == "net" || verb == "requests") {
    if (!b->js()) fail(Status::NotFound, "this page has no script");
    const auto& reqs = b->js()->network();
    if (reqs.empty()) c.o << "this page has asked the network for nothing\n";
    for (const auto& r : reqs) c.o << "  " << r << "\n";
    return;
  }

  if (verb == "log" || verb == "console") {
    const auto& run = b->script();
    c.o << run.scripts << " script(s), " << run.statements << " statement(s), " << run.dom_writes
        << " dom write(s), " << run.failed << " stopped\n";
    if (!run.error.empty()) c.o << "error: " << run.error << "\n";
    for (const auto& line : run.log) c.o << "  " << line << "\n";
    if (run.log.empty()) c.o << "  (the page logged nothing)\n";
    return;
  }

  if (verb == "dom" || verb == "tree") {
    if (!b->document().dom) fail(Status::NotFound, "no page loaded yet - browser <url> first");
    c.view();
    const std::string want = a.size() > 2 ? a[2] : std::string();
    int shown = 0;
    std::function<void(const userland::web::Node&, int)> walk =
        [&](const userland::web::Node& n, int depth) {
      if (shown > 400) return;
      if (n.kind == userland::web::Node::Kind::Element) {
        std::string line(depth * 2, ' ');
        line += "<" + n.tag + ">";
        if (n.has_attr("id")) line += " #" + n.attr("id");
        if (n.has_attr("class")) line += " ." + n.attr("class");
        if (n.has_attr("style")) line += " style=\"" + n.attr("style") + "\"";
        if (want.empty() || line.find(want) != std::string::npos) {
          c.o << line << "\n";
          ++shown;
        }
      }
      for (const auto& kid : n.children) walk(*kid, depth + 1);
    };
    walk(*b->document().dom, 0);
    c.o << shown << " element(s)" << (want.empty() ? "" : " matching '" + want + "'") << "\n";
    return;
  }

  if (verb == "boxes" || verb == "layout") {
    if (!b->document().dom) fail(Status::NotFound, "no page loaded yet - browser <url> first");
    c.view();
    const auto& doc = b->document();
    c.o << c.o.style(Header()) << W{10} << "KIND" << W{22} << "BOX" << W{16} << "ELEMENT"
        << "TEXT" << c.o.style(Reset()) << "\n";
    for (const auto& f : doc.fragments) {
      const char* kind = f.kind == userland::web::Fragment::Kind::Text ? "text"
                       : f.kind == userland::web::Fragment::Kind::Rect ? "rect"
                       : f.kind == userland::web::Fragment::Kind::Border ? "border"
                       : f.kind == userland::web::Fragment::Kind::Image ? "image"
                       : f.kind == userland::web::Fragment::Kind::Video ? "video"
                       : f.kind == userland::web::Fragment::Kind::Audio ? "audio"
                       : f.kind == userland::web::Fragment::Kind::Canvas ? "canvas" : "rule";
      std::string box = std::to_string(f.x) + "," + std::to_string(f.y) + " " +
                        std::to_string(f.w) + "x" + std::to_string(f.h);
      std::string el = "-";
      if (f.node) {
        el = "<" + (f.node->tag.empty() ? std::string("text") : f.node->tag) + ">";
        if (f.node->has_attr("id")) el += "#" + f.node->attr("id");
        if (f.node->has_attr("class")) {
          std::string cls = f.node->attr("class");
          if (cls.size() > 28) cls = cls.substr(0, 28) + "…";
          el += "." + cls;
        }
      }
      std::string txt = f.text;
      if (txt.size() > 40) txt = txt.substr(0, 40) + "…";
      c.o << W{10} << kind << W{20} << box << W{34} << el << txt << "\n";
    }
    c.o << doc.fragments.size() << " box(es)\n";
    return;
  }

  // Typing at the command surface, so the same interaction a person has with
  // the pane can be driven and inspected without a screen.
  if (verb == "type" || verb == "key") {
    if (!b->document().dom) fail(Status::NotFound, "no page loaded yet - browser <url> first");
    if (a.size() < 5) fail(Status::Invalid, "browser type <x> <y> <text>   |   browser key <x> <y> <Enter|Escape|Backspace>");
    const int cx = (int)to_u64(a[2]);
    const int cy = (int)to_u64(a[3]);
    userland::web::Node* hit = b->node_at(cx, cy);
    userland::web::Node* field = nullptr;
    for (userland::web::Node* p = hit; p; p = p->parent)
      if (p->kind == userland::web::Node::Kind::Element && (p->tag == "input" || p->tag == "textarea")) { field = p; break; }
    if (!field) fail(Status::NotFound, "nothing you can type into at that point");
    if (!b->js()) fail(Status::NotFound, "this page has no script");
    int wrote = 0;
    if (verb == "type") {
      std::string text = a[4];
      for (size_t i = 5; i < a.size(); ++i) text += " " + a[i];
      std::string v = field->tag == "textarea" && !field->has_attr("value") ? field->inner_text()
                                                                           : field->attr("value");
      v += text;
      bool found = false;
      for (auto& at : field->attrs) if (at.name == "value") { at.value = v; found = true; break; }
      if (!found) { userland::web::Attr at; at.name = "value"; at.value = v; field->attrs.push_back(at); }
      if (field->tag == "textarea") {
        field->children.clear();
        auto t = std::make_unique<userland::web::Node>();
        t->kind = userland::web::Node::Kind::Text;
        t->text = v;
        t->parent = field;
        field->children.push_back(std::move(t));
      }
      b->js()->dispatch(field, "input", &wrote);
      c.o << "typed into <" << field->tag << ">: " << v << "\n";
    } else {
      userland::EventInit init;
      init.key = a[4];
      bool prevented = false;
      const int ran = b->js()->dispatch(field, "keydown", &wrote, &init, &prevented);
      c.o << "key " << init.key << " to <" << field->tag << ">: " << ran << " handler(s) ran, "
          << (prevented ? "page took it" : "page let it through") << "\n";
    }
    c.o << "  " << wrote << " dom change(s)\n";
    if (!b->js()->last_error().empty()) c.o << "  handler failed: " << b->js()->last_error() << "\n";
    // Typing changed the field's value even when no handler wrote anything,
    // so the page is laid out again either way; otherwise the text view kept
    // showing the placeholder over what had just been typed.
    if (wrote > 0 || verb == "type") b->relayout(m, viewport);
    for (const auto& line : b->as_text(columns)) c.o << line << "\n";
    return;
  }

  if (verb == "click" || verb == "tap") {
    if (!b->document().dom) fail(Status::NotFound, "no page loaded yet - browser <url> first");
    if (a.size() < 4) fail(Status::Invalid, "browser click <x> <y>");
    const int cx = (int)to_u64(a[2]);
    const int cy = (int)to_u64(a[3]);
    userland::web::Node* target = b->node_at(cx, cy);
    c.o << "at (" << cx << ", " << cy << "): "
        << (target ? ("<" + target->tag + ">" + (target->has_attr("id") ? " #" + target->attr("id") : std::string()))
                   : std::string("nothing"))
        << "\n";
    if (!b->js()) fail(Status::NotFound, "this page has no script");
    c.o << "  " << b->js()->listener_count() << " handler(s) registered\n";
    int wrote = 0;
    const int ran = b->js()->dispatch(target, "click", &wrote);
    c.o << "  " << ran << " handler(s) ran, " << wrote << " dom change(s)\n";
    if (!b->js()->last_error().empty()) c.o << "  handler failed: " << b->js()->last_error() << "\n";
    if (wrote > 0) {
      b->relayout(m, viewport);
      c.o << "  laid out again: " << b->document().fragments.size() << " box(es)\n";
    }
    for (const auto& line : b->as_text(columns)) c.o << line << "\n";
    return;
  }

  if (verb == "follow" || verb == "back" || verb == "forward" || verb == "page" || verb == "reload") {
    if (!b->document().dom && verb != "page")
      fail(Status::NotFound, "no page loaded yet — browser <url> first");
    Result<void> r = Ok();
    if (verb == "follow") {
      if (a.size() < 3) fail(Status::Invalid, "browser follow <n>");
      uint64_t n = 0;
      for (char ch : a[2]) {
        if (ch < '0' || ch > '9') fail(Status::Invalid, "browser follow <n>: '" + a[2] + "' is not a number");
        n = n * 10 + (uint64_t)(ch - '0');
      }
      r = b->follow((size_t)n, m, viewport);
    } else if (verb == "back") {
      r = b->back(m, viewport);
    } else if (verb == "forward") {
      r = b->forward(m, viewport);
    } else if (verb == "reload") {
      r = b->reload(m, viewport);
    }
    if (!r.ok()) fail(r.status(), r.error().message);
    if (!b->document().dom) fail(Status::NotFound, "no page loaded yet — browser <url> first");
    print_page(c, *b, columns);
    return;
  }

  std::string url = a[1];
  if (url.find("://") == std::string::npos) url = "http://" + url;
  c.o << "fetching " << url << " …\n";
  c.o.flush();
  auto r = b->go(url, m, viewport);
  if (!r.ok())
    fail(r.status(), "browser: " + (r.error().message.empty() ? ("could not load " + url) : r.error().message));

  // The page's own pictures, decoded here so `browser` reports what it could
  // and could not draw rather than silently listing alt text.
  b->fetch_images(12);
  print_page(c, *b, columns);

  // What the machine now knows about the page, kept the way `sense` keeps it,
  // so a page read in the browser is a page this machine can be asked about.
  {
    std::string text;
    for (const auto& line : b->as_text(120)) { text += line; text += "\n"; }
    if (!text.empty()) {
      StoredObject o;
      o.payload = ByteBuffer(text.begin(), text.end());
      o.embedding = embed_text(b->document().title + "\n" + text);
      o.metadata.push_back({"url", b->document().url});
      o.metadata.push_back({"title", b->document().title});
      o.metadata.push_back({"seen", "browser"});
      auto id = c.me.store(std::move(o));
      if (id.ok()) {
        c.o << "\nkept as object " << id->short_str() << "\n";
        c.produced(id->str());
      }
    }
  }
}

void cmd_gui(Ctx& c, const Args& a) {
  if (a.size() >= 2 && (a[1] == "desktop" || a[1] == "native" || a[1] == "run")) {
    bool force_tui = (a.size() >= 3 && (a[2] == "--tui" || a[2] == "-t"));
    // On bare metal the desktop is the substrate's business, not this command's:
    // the screen it composites onto is the machine's own framebuffer and the
    // mouse is a device the HAL drives, neither of which exists here. The host
    // hook runs it (metal/main.cpp -> metal/desktop.cpp).
#if defined(AIOS_METAL)
    if (force_tui) { c.o << "there is no terminal under this kernel to fall back to\n"; return; }
    if (!c.s.host) { c.o << "this substrate has no desktop\n"; return; }
    auto hr = c.s.host({"desktop"}, c.o, c.me);
    if (!hr.ok()) c.o << "desktop: " << hr.error().str() << "\n";
    return;
#else
    aios::userland::HardwareCompositor compositor(1280, 800);
    auto dispatcher = [&c](const std::string& cmd, aios::userland::OutputCallback on_line) {
      // Split the captured chunks into lines by hand: this runs on every
      // substrate, and a bare-metal kernel has no locale for the stream
      // machinery to initialise.
      std::string pending;
      {
        Out captured_out([&](const std::string& chunk) {
          for (char ch : chunk) {
            if (ch == '\n') { on_line(pending); pending.clear(); }
            else if (ch != '\r') pending.push_back(ch);
          }
        });
        captured_out.set_colour(false);
        auto* old_out = c.s.out;
        c.s.out = &captured_out;
        auto r = run_line(c.s, cmd);
        captured_out.flush();
        c.s.out = old_out;
        if (!r.ok()) on_line("error: " + r.error().str());
      }
      if (!pending.empty()) on_line(pending);
    };
    c.o << "Launching AIOS Native Desktop Compositor Workstation...\n";
    compositor.run_desktop(c.k, dispatcher, force_tui);
    return;
#endif
  }
  c.o << c.o.style(Bold()) << "AIOS desktop" << c.o.style(Reset()) << "\n"
      << "  compositor  : aios::userland::HardwareCompositor - SDL2 on a host, this\n"
      << "                machine's own framebuffer with PS/2 mouse and keyboard on\n"
      << "                bare metal. One window manager, one set of input calls.\n"
      << "  start it    : 'gui desktop' (or 'gui desktop --tui') here, or 'aios desktop'\n"
      << "                from a host shell. A bare 'gui' or 'desktop' prints this.\n"
      << "  keys        : (the live table: the defaults plus " << userland::key_bindings_path(c.k.config().root) << ")\n";
  // The function keys as the desktop will actually bind them, read from the
  // same table it uses -- this text used to list four panes from long ago.
  {
    userland::KeyBindings keys = userland::KeyBindings::defaults();
    auto rd = c.k.host().read_file(userland::key_bindings_path(c.k.config().root));   // host-io: the machine's key-binding table (userland::key_bindings_path)
    if (rd.ok()) (void)keys.apply_text(std::string(reinterpret_cast<const char*>(rd->data()), rd->size()));
    for (int fk = hal::KeyF1; fk <= hal::KeyF12; ++fk) {
      const userland::UiAction act = keys.action_for(fk);
      if (act == userland::UiAction::None) continue;
      std::string name = userland::chord_name(fk);
      if (name.size() < 4) name.resize(4, ' ');
      c.o << "                " << name << " " << userland::action_help(act) << "\n";
    }
  }
  c.o << "                In the editor, F12 is go-to-definition and F1 is hover.\n"
      << "  look        : the assay console's palette and layout grammar, so the\n"
      << "                machine looks the same whether you drive it from the\n"
      << "                console or sit in front of it\n";
}



void show_entity(Ctx& c, const Entity& e) {
  c.o << e.id.short_str() << " (" << e.id.str() << ")\n"
      << "  label=" << e.label << " status=" << entity_status_name(e.status) << " gen=" << e.generation
      << " states=" << e.states.size() << " memory=" << e.memory.size() << " caps=" << e.capabilities.size()
      << " checkpoints=" << e.checkpoints << " restores=" << e.restores;
  if (!e.execution.is_null()) c.o << " execution=" << e.execution.short_str() << "@" << e.execution_position;
  c.o << "\n";
  for (auto& r : e.relationships) c.o << "    " << relation_kind_name(r.kind) << " → " << r.peer.short_str() << "\n";
  if (const MindBinding* m = c.k.mind(e.id)) c.o << "    mind: " << m->mind->name() << " cycles=" << m->cycles << (m->output.is_null() ? "" : " output=" + m->output.short_str()) << "\n";
}

void cmd_entity(Ctx& c, const Args& a) {
  if (a.size() < 2) fail(Status::Invalid, "entity create|list|show|relate|terminate");
  const std::string& sub = a[1];
  if (sub == "create") {
    if (a.size() < 3) fail(Status::Invalid, "entity create <label> [parent]");
    EntityID parent; if (a.size() > 3) parent = c.r.as<EntityID>(a[3]);
    auto id = need(c.me.spawn(a[2], parent));
    c.produced(id.str());
    c.o << "entity " << id.str() << " (" << a[2] << ")\n";
  } else if (sub == "list") {
    c.view();
    c.o << c.o.style(Header())
        << W{14} << "ENTITY ID"
        << W{16} << "LABEL"
        << W{10} << "STATUS"
        << W{6}  << "GEN"
        << W{8}  << "STATES"
        << W{6}  << "MEM"
        << W{6}  << "CAPS"
        << "DETAILS" << c.o.style(Reset()) << "\n";
    for (auto& e : c.k.entities().list()) {
      c.o << W{14} << e.id.short_str()
          << W{16} << (e.label.size() > 15 ? e.label.substr(0, 12) + "..." : e.label)
          << W{10} << entity_status_name(e.status)
          << W{6}  << e.generation
          << W{8}  << e.states.size()
          << W{6}  << e.memory.size()
          << W{6}  << e.capabilities.size();
      if (!e.execution.is_null()) c.o << "exec=" << e.execution.short_str() << "@" << e.execution_position << " ";
      if (const MindBinding* m = c.k.mind(e.id)) c.o << "mind=" << m->mind->name() << " ";
      c.o << "\n";
    }
  } else if (sub == "show") {
    if (a.size() < 3) fail(Status::Invalid, "entity show <e>");
    c.view();
    show_entity(c, need(c.k.entities().copy(c.r.as<EntityID>(a[2]))));
  } else if (sub == "relate") {
    if (a.size() < 5) fail(Status::Invalid, "entity relate <a> <b> <kind>");
    uint32_t kind; if (!parse_relation_kind(a[4], kind)) fail(Status::Invalid, "unknown relation kind");
    need(c.as(a[2]).relate(c.r.as<EntityID>(a[3]), kind));
    c.o << "related\n";
  } else if (sub == "terminate") {
    if (a.size() < 3) fail(Status::Invalid, "entity terminate <e>");
    EntityID id = c.r.as<EntityID>(a[2]);
    need(c.me.terminate(id));
    c.o << "terminated " << id.short_str() << " (memory released, capabilities revoked, channels destroyed)\n";
  } else fail(Status::Invalid, "unknown: entity " + sub);
}

ResourceRef parse_resource(Ctx& c, const std::string& kind, const std::string& id) {
  ResourceRef r;
  if (!parse_resource_kind(kind, r.kind)) fail(Status::Invalid, "unknown resource kind '" + kind + "' (memory|compute|channel|storage|entity|network|state|kernel)");
  if (id != "*") {
    if (r.kind == ResourceKind::Kernel) {
      std::string exp = c.r.expand(id);
      Id128 parsed;
      if (Id128::parse(exp, parsed)) r.id = parsed;
      else if (exp == "kernel") r.id = c.k.self();
      else r.id = disk_id(exp);
    } else {
      r.id = c.r.id(id);
    }
  }
  return r;
}

void cmd_cap(Ctx& c, const Args& a) {
  if (a.size() < 2) fail(Status::Invalid, "cap grant|request|delegate|revoke|list|find|check");
  const std::string& sub = a[1];
  if (sub == "grant") {
    if (a.size() < 6) fail(Status::Invalid, "cap grant <e> <kind> <id|*> <rights> [ttl_s] [scope_bytes]");
    uint16_t rights; if (!parse_rights(a[5], rights)) fail(Status::Invalid, "bad rights '" + a[5] + "'");
    uint64_t ttl = a.size() > 6 ? to_u64(a[6]) * 1000000000ull : 0;
    uint64_t scope = a.size() > 7 ? to_u64(a[7]) : 0;
    auto id = need(c.me.grant(c.r.as<EntityID>(a[2]), parse_resource(c, a[3], a[4]), rights, ttl, scope));
    c.produced(id.str());
    c.o << "capability " << id.str() << " (derived from the console's authority)\n";
  } else if (sub == "request") {
    // "I need X." The kernel answers from the console's current authority.
    if (a.size() < 5) fail(Status::Invalid, "cap request <kind> <id|*> <rights> [ttl_s] [confirm=<id>]");
    uint16_t rights; if (!parse_rights(a[4], rights)) fail(Status::Invalid, "bad rights '" + a[4] + "'");
    uint64_t ttl = 0;
    // confirm=<id>: the resource named a second time, which is what a
    // kernel:destroy over a disk needs (cap request kernel <disk> destroy confirm=<disk>).
    bool confirmed = false;
    for (size_t i = 5; i < a.size(); ++i) {
      if (a[i].rfind("confirm=", 0) == 0) {
        if (a[i].substr(8) != c.r.expand(a[3])) fail(Status::Invalid, "confirm= must name " + a[3] + " again");
        confirmed = true;
      } else if (ttl == 0) ttl = to_u64(a[i]) * 1000000000ull;
    }
    auto id = need(c.me.request(parse_resource(c, a[2], a[3]), rights, ttl, confirmed));
    c.produced(id.str());
    const Capability* cap = c.k.capabilities().get(id);
    c.o << "the kernel permits it: capability " << id.str() << (cap ? " " + cap->resource.str() + " " + rights_str(cap->rights) : std::string()) << "\n";
  } else if (sub == "delegate") {
    if (a.size() < 6) fail(Status::Invalid, "cap delegate <e> <cap> <to> <rights> [ttl_s]");
    uint16_t rights; if (!parse_rights(a[5], rights)) fail(Status::Invalid, "bad rights");
    uint64_t ttl = a.size() > 6 ? to_u64(a[6]) * 1000000000ull : 0;
    auto id = need(c.as(a[2]).delegate(c.r.as<CapabilityID>(a[3]), c.r.as<EntityID>(a[4]), rights, ttl));
    c.produced(id.str());
    c.o << "capability " << id.str() << " (delegated)\n";
  } else if (sub == "revoke") {
    if (a.size() < 3) fail(Status::Invalid, "cap revoke <cap>");
    need(c.me.revoke(c.r.as<CapabilityID>(a[2])));
    c.o << "revoked (and every capability delegated from it)\n";
  } else if (sub == "list" || sub == "mine") {
    EntityID e = c.me.self();
    if (sub == "list") { c.view(); e = EntityID{}; if (a.size() > 2) e = c.r.as<EntityID>(a[2]); }
    c.o << c.o.style(Header())
        << W{14} << "CAP ID"
        << W{14} << "HOLDER"
        << W{22} << "RESOURCE"
        << W{22} << "RIGHTS"
        << "DETAILS" << c.o.style(Reset()) << "\n";
    for (auto& cap : c.k.capabilities().list(e)) {
      c.o << W{14} << cap.id.short_str() << W{14} << cap.holder.short_str() << W{22} << cap.resource.str() << W{22} << rights_str(cap.rights);
      if (cap.scope_limit && cap.resource.kind == ResourceKind::Kernel)
        c.o << "  uses=" << fmt_u64(cap.scope_used) << "/" << fmt_u64(cap.scope_limit);
      else if (cap.scope_limit) c.o << "  scope=" << fmt_bytes(cap.scope_used) << "/" << fmt_bytes(cap.scope_limit);
      if (cap.expires_ns) c.o << "  expires=" << (cap.expires_ns > c.k.now() ? "+" + fmt_ns(cap.expires_ns - c.k.now()) : "EXPIRED");
      if (!cap.parent.is_null()) c.o << "  from=" << cap.parent.short_str();
      c.o << "\n";
    }
  } else if (sub == "find") {
    if (a.size() < 4) fail(Status::Invalid, "cap find <e> <kind> [id|*]");
    c.view();
    ResourceRef want = parse_resource(c, a[3], a.size() > 4 ? a[4] : "*");
    for (auto& cap : c.k.capabilities().list(c.r.as<EntityID>(a[2]))) {
      if (cap.resource.kind == want.kind && cap.resource.id == want.id) { c.produced(cap.id.str()); c.o << "capability " << cap.id.str() << " " << cap.resource.str() << " " << rights_str(cap.rights) << "\n"; return; }
    }
    fail(Status::NotFound, "no capability of that shape");
  } else if (sub == "check") {
    if (a.size() < 6) fail(Status::Invalid, "cap check <e> <kind> <id|*> <right>");
    uint16_t rights; if (!parse_rights(a[5], rights)) fail(Status::Invalid, "bad rights");
    c.view();
    bool ok = c.k.capabilities().check(c.r.as<EntityID>(a[2]), parse_resource(c, a[3], a[4]), rights, c.k.now());
    c.o << (ok ? "permitted" : "denied") << "\n";
    if (!ok) fail(Status::Denied, "capability check failed");
  } else fail(Status::Invalid, "unknown: cap " + sub);
}

// The language is a tool of the console, not an authority: it inspects the
// machine through `sys` and cannot act through the console entity, so running a
// program needs view() and nothing more.
void cmd_grain(Ctx& c, const Args& a) {
  c.view();
  need(run_grain_command(c.k, c.o, a));
}

void cmd_mem(Ctx& c, const Args& a) {
  if (a.size() < 2) fail(Status::Invalid, "mem alloc|write|read|migrate|share|release|list");
  const std::string& sub = a[1];
  if (sub == "alloc") {
    if (a.size() < 4) fail(Status::Invalid, "mem alloc <e> <bytes> [system|shared|persistent]");
    MemoryType t = MemoryType::System;
    if (a.size() > 4 && !parse_memory_type(a[4], t)) fail(Status::Invalid, "unknown memory type");
    auto id = need(c.as(a[2]).allocate(to_u64(a[3]), t));
    c.produced(id.str());
    c.o << "memory " << id.str() << " (" << fmt_bytes(to_u64(a[3])) << " " << memory_type_name(t) << ")\n";
  } else if (sub == "write") {
    if (a.size() < 6) fail(Status::Invalid, "mem write <e> <mem> <offset> <text|hex:..>");
    auto b = parse_bytes(a[5]);
    need(c.as(a[2]).write(c.r.as<MemoryID>(a[3]), to_u64(a[4]), b));
    c.o << "wrote " << b.size() << " bytes\n";
  } else if (sub == "read") {
    if (a.size() < 6) fail(Status::Invalid, "mem read <e> <mem> <offset> <n>");
    auto b = need(c.as(a[2]).read(c.r.as<MemoryID>(a[3]), to_u64(a[4]), to_u64(a[5])));
    c.o << hex(b, 48) << "  " << c.bytes_line(b) << "\n";
    c.s.vars["read"] = std::string(b.begin(), b.end());
  } else if (sub == "migrate") {
    if (a.size() < 5) fail(Status::Invalid, "mem migrate <e> <mem> <type>");
    MemoryType t; if (!parse_memory_type(a[4], t)) fail(Status::Invalid, "unknown memory type");
    MemoryID m = c.r.as<MemoryID>(a[3]);
    need(c.as(a[2]).migrate(m, t));
    const MemoryObject* o = c.k.memory().lookup(m);
    c.o << "memory " << m.short_str() << " now " << memory_type_name(o->type) << " gen=" << o->generation << (o->mapping.path.empty() ? "" : " backing=" + o->mapping.path) << "\n";
  } else if (sub == "share") {
    if (a.size() < 6) fail(Status::Invalid, "mem share <e> <mem> <to> <rights>");
    uint16_t rights; if (!parse_rights(a[5], rights)) fail(Status::Invalid, "bad rights");
    auto id = need(c.as(a[2]).share(c.r.as<MemoryID>(a[3]), c.r.as<EntityID>(a[4]), rights));
    c.produced(id.str());
    c.o << "capability " << id.str() << " (shared " << rights_str(rights) << ")\n";
  } else if (sub == "release") {
    if (a.size() < 4) fail(Status::Invalid, "mem release <e> <mem>");
    need(c.as(a[2]).release(c.r.as<MemoryID>(a[3])));
    c.o << "released\n";
  } else if (sub == "list") {
    c.view();
    EntityID e; if (a.size() > 2) e = c.r.as<EntityID>(a[2]);
    c.o << c.o.style(Header())
        << W{14} << "MEMORY ID"
        << W{12} << "TYPE"
        << W{12} << "SIZE"
        << W{14} << "OWNER"
        << W{6}  << "GEN"
        << "BACKING" << c.o.style(Reset()) << "\n";
    for (auto& m : c.k.memory().list(e)) {
      c.o << W{14} << m.id.short_str()
          << W{12} << memory_type_name(m.type)
          << W{12} << fmt_bytes(m.size)
          << W{14} << m.owner.short_str()
          << W{6}  << m.generation
          << (m.mapping.path.empty() ? "" : m.mapping.path) << "\n";
    }
  } else if (sub == "compact" || sub == "gc") {
    c.view();
    // The entity registry is what makes an orphan visible: memory whose owner
    // no longer exists is unreachable by every other path, including terminate.
    auto res = c.k.memory().compact_garbage(
        [&](const EntityID& e) { return c.k.entities().exists(e); });
    c.o << "Memory Fabric Garbage Collection: checked " << res.objects_checked << " object(s), released " << res.objects_released << " orphaned object(s) (" << fmt_bytes(res.bytes_reclaimed) << " reclaimed)\n";
  } else fail(Status::Invalid, "unknown: mem " + sub);
}

void cmd_watchdog(Ctx& c, const Args& a) {
  c.view();
  auto st = c.k.watchdog().check_health();
  c.o << c.o.style(Header()) << "AIOS Self-Healing System Watchdog" << c.o.style(Reset()) << "\n";
  c.o << "  Checks Performed    : " << st.checks_performed << "\n";
  c.o << "  Recoveries Triggered: " << st.recoveries_triggered << "\n";
  c.o << "  Heap Memory Used    : " << fmt_bytes(st.heap_kib * 1024) << "\n";
  c.o << "  Pipe Lock Status    : " << (st.lock_pipe_ok ? "OK" : "held, no owner recorded (left to aos.sh)") << "\n";
  if (!st.last_remedy.empty()) {
    c.o << "  Last Remedy Action  : " << st.last_remedy << "\n";
  }
  if (!st.event_log.empty()) {
    c.o << "\n" << c.o.style(Bold()) << "Recent Watchdog Events:" << c.o.style(Reset()) << "\n";
    for (const auto& ev : st.event_log) {
      c.o << "  · " << ev << "\n";
    }
  }
}

void cmd_feed(Ctx& c, const Args& a) {
  c.view();
  if (a.size() < 2) fail(Status::Invalid, "feed ingest <url> [as=$e] | feed list [url]");
  const std::string& sub = a[1];
  if (sub == "ingest" || sub == "fetch") {
    if (a.size() < 3) fail(Status::Invalid, "feed ingest <url>");
    std::string url = c.r.expand(a[2]);
    c.o << "Ingesting RSS/Atom feed from " << url << "...\n";
    // With the caller's authority (or as=$e's): network to fetch, storage to keep.
    Surface actor = has_kv(a, "as") ? c.as(kv(a, "as")) : c.me;
    auto res = userland::ingest_rss_feed(c.k, &actor, url);
    if (!res.ok()) fail(res.error().status, res.error().message);
    c.o << c.o.style(Header()) << "Feed: " << res->feed_title << c.o.style(Reset()) << "\n";
    c.o << "Parsed " << res->items.size() << " item(s); stored " << res->items_stored << " new object(s)";
    if (res->items_already_kept) c.o << ", " << res->items_already_kept << " already kept";
    c.o << "\n\n";
    for (size_t i = 0; i < res->items.size() && i < 6; ++i) {
      const auto& item = res->items[i];
      c.o << "  [" << (i + 1) << "] " << c.o.style(Bold()) << item.title << c.o.style(Reset()) << "\n";
      c.o << "      Link: " << item.link << "\n";
      if (!item.pub_date.empty()) c.o << "      Date: " << item.pub_date << "\n";
    }
  } else if (sub == "list" || sub == "ls") {
    std::string url = a.size() > 2 ? c.r.expand(a[2]) : std::string();
    auto items = userland::stored_feed_items(c.k, url);
    c.o << c.o.style(Header()) << "Kept feed items" << (url.empty() ? "" : " from " + url) << c.o.style(Reset())
        << " (" << items.size() << ")\n";
    for (const auto& it : items) {
      c.o << "  " << c.o.style(Bold()) << it.title << c.o.style(Reset()) << "\n      " << it.link;
      if (!it.pub_date.empty()) c.o << "  " << it.pub_date;
      if (url.empty()) c.o << "  [" << it.description << "]";
      c.o << "\n";
    }
  } else fail(Status::Invalid, "feed ingest <url> | feed list [url]");
}

void show_state(Ctx& c, const StateObject& st) {
  c.o << W{14} << st.id.short_str()
      << W{14} << state_kind_name(st.kind)
      << W{14} << st.owner.short_str()
      << W{6}  << st.generation
      << c.bytes_line(st.payload) << "\n";
}

// ASQL capability credentials. attenuate needs no key (anyone holding a
// credential can narrow it); mint and show use this machine's capability key.
void cmd_asqlcap(Ctx& c, const Args& a) {
  const char* usage = "asqlcap mint tables=db.t,db.* rights=read[,write,...] [ttl=<s>] [rows=<n>] | "
                      "asqlcap attenuate <credential> [tables=..] [rights=..] [ttl=<s>] [rows=<n>] | asqlcap show <credential> | "
                      "asqlcap user add <name> password=<pw> tables=db.* [rights=..] [ttl=<s>] [rows=<n>] | "
                      "asqlcap user password <name> password=<pw> | asqlcap user del <name> | asqlcap user list";
  if (a.size() < 2) fail(Status::Invalid, usage);
  const std::string& sub = a[1];
  auto read_grant = [&](userland::AsqlGrant& g, bool need_scope) {
    std::string err;
    if (has_kv(a, "tables")) { if (!userland::asql_parse_scopes(kv(a, "tables"), &g.scopes, &err)) fail(Status::Invalid, err); }
    else if (need_scope) fail(Status::Invalid, "tables= is required");
    if (has_kv(a, "rights")) { if (!userland::asql_parse_rights(kv(a, "rights"), &g.rights, &err)) fail(Status::Invalid, err); }
    else if (need_scope) g.rights = 1;
    if (has_kv(a, "ttl")) g.ttl_ns = to_u64(kv(a, "ttl")) * 1000000000ull;
    if (has_kv(a, "rows")) g.rows_cap = to_u64(kv(a, "rows"));
  };
  auto open_guard = [&]() {
    const std::string root = c.k.config().root;
    const std::string key = userland::asql_load_capability_key(root);
    if (key.empty()) fail(Status::Invalid, "no capability key: set AIOS_CAPABILITY_KEY or make " + root + "/capability.key writable");
    return std::make_unique<sidecar::CapabilityGuard>(key, root + "/capability-state");
  };
  std::string err, out;
  if (sub == "mint") {
    userland::AsqlGrant g;
    read_grant(g, true);
    if (!has_kv(a, "ttl")) g.ttl_ns = 3600ull * 1000000000ull;
    auto guard = open_guard();
    if (!userland::asql_mint(*guard, g, &out, &err)) fail(Status::Invalid, err);
    c.o << out << "\n";
  } else if (sub == "attenuate") {
    if (a.size() < 3) fail(Status::Invalid, usage);
    userland::AsqlGrant g;
    read_grant(g, false);
    if (!userland::asql_attenuate(a[2], g, &out, &err)) fail(Status::Invalid, err);
    c.o << out << "\n";
  } else if (sub == "show") {
    if (a.size() < 3) fail(Status::Invalid, usage);
    auto guard = open_guard();
    userland::AsqlAuthority au;
    if (!userland::asql_verify(*guard, a[2], &au, &err)) fail(Status::Denied, err);
    c.o << "rights: " << userland::asql_rights_text(au.rights) << "\n";
    for (const auto& s : au.scopes) c.o << "scope:  " << s.db << "." << s.table << "\n";
    c.o << "expires_ns: " << au.expires_at_ns << "\nrows: " << (au.rows_cap ? std::to_string(au.rows_cap) : std::string("unlimited")) << "\n";
  } else if (sub == "user") {
    // The same file the running server reads (<root>/capability-users), so a
    // user added here can log in at once. A user removed here cannot log in
    // again, but a session it already holds runs until it reconnects: DROP
    // USER over the wire is what cuts a live session off.
    if (a.size() < 3) fail(Status::Invalid, usage);
    const std::string& op = a[2];
    userland::AsqlUserStore store(c.k.config().root + "/capability-users");
    if (op == "list") {
      auto guard = open_guard();
      for (const auto& u : store.list()) {
        c.o << u.name;
        userland::AsqlAuthority au;
        if (!userland::asql_verify(*guard, u.credential, &au, &err)) { c.o << "  " << err << "\n"; continue; }
        c.o << "  rights=" << userland::asql_rights_text(au.rights) << " tables=";
        for (size_t i = 0; i < au.scopes.size(); ++i) c.o << (i ? "," : "") << au.scopes[i].db << "." << au.scopes[i].table;
        c.o << " expires_ns=" << au.expires_at_ns << " rows=" << (au.rows_cap ? std::to_string(au.rows_cap) : std::string("unlimited")) << "\n";
      }
      return;
    }
    if (a.size() < 4) fail(Status::Invalid, usage);
    const std::string& name = a[3];
    if (!userland::asql_valid_user_name(name)) fail(Status::Invalid, "a user name is 1-32 letters, digits, _ - . and is not 'cap'");
    userland::AsqlUser user;
    const bool exists = store.find(name, &user);
    if (op == "add") {
      if (exists) fail(Status::Invalid, "user '" + name + "' exists");
      if (!has_kv(a, "password") || kv(a, "password").empty()) fail(Status::Invalid, "password= is required");
      userland::AsqlGrant g;
      read_grant(g, true);
      auto guard = open_guard();
      user.name = name;
      user.password_hash = userland::asql_password_hash(kv(a, "password"));
      if (!userland::asql_mint(*guard, g, &user.credential, &err)) fail(Status::Invalid, err);
      if (!store.put(user, &err)) fail(Status::Invalid, err);
      c.o << "user " << name << " added\n";
    } else if (op == "password") {
      if (!exists) fail(Status::Invalid, "no user '" + name + "'");
      if (!has_kv(a, "password") || kv(a, "password").empty()) fail(Status::Invalid, "password= is required");
      user.password_hash = userland::asql_password_hash(kv(a, "password"));
      if (!store.put(user, &err)) fail(Status::Invalid, err);
      c.o << "user " << name << " password changed\n";
    } else if (op == "del") {
      if (!exists) fail(Status::Invalid, "no user '" + name + "'");
      if (!store.erase(name, &err)) fail(Status::Invalid, err);
      c.o << "user " << name << " removed (a session already open under it runs until it reconnects; DROP USER over the wire ends it now)\n";
    } else {
      fail(Status::Invalid, usage);
    }
  } else {
    fail(Status::Invalid, usage);
  }
}

void cmd_state(Ctx& c, const Args& a) {
  if (a.size() < 2) fail(Status::Invalid, "state create|set|get|attach|keep|list");
  const std::string& sub = a[1];
  if (sub == "create") {
    if (a.size() < 4) fail(Status::Invalid, "state create <e> <kind> [text]");
    StateKind k; if (!parse_state_kind(a[3], k)) fail(Status::Invalid, "kind: working|persistent|context|relationship|execution");
    ByteBuffer p; if (a.size() > 4) p = parse_bytes(a[4]);
    Surface owner = c.as(a[2]);
    auto id = need(owner.create_state(k, p));
    c.produced(id.str());
    c.o << "state " << id.str() << " (" << state_kind_name(k) << ")\n";
    // A `persistent` state is kept in the root when its owner outlives the
    // process (a resident); otherwise say so rather than imply it.
    if (k == StateKind::Persistent) {
      auto kept = owner.keep_state(id);
      c.o << (kept.ok() ? "  kept in the root: it is there on the next boot\n"
                        : "  not kept across boots: " + kept.error().message + "\n");
    }
  } else if (sub == "keep") {
    if (a.size() < 4) fail(Status::Invalid, "state keep <e> <s>");
    need(c.as(a[2]).keep_state(c.r.as<StateID>(a[3])));
    c.o << "kept in the root: it is there on the next boot\n";
  } else if (sub == "set") {
    if (a.size() < 5) fail(Status::Invalid, "state set <e> <s> <text>");
    need(c.as(a[2]).set_state(c.r.as<StateID>(a[3]), parse_bytes(a[4])));
    c.o << "state updated gen=" << c.k.states().get(c.r.as<StateID>(a[3]))->generation << "\n";
  } else if (sub == "get") {
    if (a.size() < 4) fail(Status::Invalid, "state get <e> <s>");
    auto st = need(c.as(a[2]).state(c.r.as<StateID>(a[3])));
    show_state(c, st);
    c.s.vars["read"] = std::string(st.payload.begin(), st.payload.end());
  } else if (sub == "attach") {
    if (a.size() < 5) fail(Status::Invalid, "state attach <e> <s> <mem>");
    need(c.as(a[2]).attach(c.r.as<StateID>(a[3]), c.r.as<MemoryID>(a[4])));
    c.o << "attached\n";
  } else if (sub == "list") {
    c.view();
    EntityID e; if (a.size() > 2) e = c.r.as<EntityID>(a[2]);
    c.o << c.o.style(Header())
        << W{14} << "STATE ID"
        << W{14} << "KIND"
        << W{14} << "OWNER"
        << W{6}  << "GEN"
        << "PAYLOAD" << c.o.style(Reset()) << "\n";
    for (auto& st : c.k.states().list(e)) show_state(c, st);
  } else fail(Status::Invalid, "unknown: state " + sub);
}

template <typename T> void write_values(Ctx& c, Surface& s, const Tensor& t, const Args& vals) {
  std::vector<T> v(t.elements(), T(0));
  for (size_t i = 0; i < vals.size() && i < v.size(); ++i) v[i] = (T)to_f64(vals[i]);
  if (vals.size() == 1 && v.size() > 1) for (auto& x : v) x = v[0];
  need(s.write(t.memory, t.offset, ByteSpan(reinterpret_cast<const uint8_t*>(v.data()), v.size() * sizeof(T))));
}
template <typename T> void print_values(Ctx& c, const ByteBuffer& b, uint64_t n) {
  const T* v = reinterpret_cast<const T*>(b.data());
  uint64_t count = std::min<uint64_t>(n, b.size() / sizeof(T));
  c.o << "[";
  for (uint64_t i = 0; i < count && i < 64; ++i) { if (i) c.o << " "; if constexpr (std::is_floating_point_v<T>) c.o << Prec{5} << (double)v[i]; else c.o << (long long)v[i]; }
  if (count > 64) c.o << " … (" << count << " values)";
  c.o << "]\n";
}

void cmd_tensor(Ctx& c, const Args& a) {
  if (a.size() < 2) fail(Status::Invalid, "tensor create|show");
  const std::string& sub = a[1];
  if (sub == "create") {
    if (a.size() < 5) fail(Status::Invalid, "tensor create <e> <dtype> <shape> [values...]");
    DType dt; if (!parse_dtype(a[3], dt)) fail(Status::Invalid, "dtype: f32|f64|i32|i64|u8");
    Surface s = c.as(a[2]);
    Tensor t = need(s.tensor(dt, parse_shape(a[4])));
    Args vals(a.begin() + 5, a.end());
    if (!vals.empty()) {
      switch (dt) {
        case DType::F32: write_values<float>(c, s, t, vals); break; case DType::F64: write_values<double>(c, s, t, vals); break;
        case DType::I32: write_values<int32_t>(c, s, t, vals); break; case DType::I64: write_values<int64_t>(c, s, t, vals); break; case DType::U8: write_values<uint8_t>(c, s, t, vals); break;
      }
    }
    c.produced(t.memory.str());
    c.o << "tensor " << t.describe() << " memory " << t.memory.str() << "\n";
  } else if (sub == "show") {
    if (a.size() < 6) fail(Status::Invalid, "tensor show <e> <mem> <dtype> <shape>");
    DType dt; if (!parse_dtype(a[4], dt)) fail(Status::Invalid, "bad dtype");
    auto shape = parse_shape(a[5]);
    Tensor t; t.dtype = dt; t.shape = shape; t.memory = c.r.as<MemoryID>(a[3]);
    auto b = need(c.as(a[2]).read(t.memory, 0, t.bytes()));
    c.o << t.describe() << " = ";
    switch (dt) {
      case DType::F32: print_values<float>(c, b, t.elements()); break; case DType::F64: print_values<double>(c, b, t.elements()); break;
      case DType::I32: print_values<int32_t>(c, b, t.elements()); break; case DType::I64: print_values<int64_t>(c, b, t.elements()); break; case DType::U8: print_values<uint8_t>(c, b, t.elements()); break;
    }
  } else fail(Status::Invalid, "unknown: tensor " + sub);
}

Tensor parse_tensor_ref(Ctx& c, const std::string& spec) {
  // <mem>:<dtype>:<shape>
  std::vector<std::string> parts = split(spec, ':');
  if (parts.size() != 3) fail(Status::Invalid, "tensor ref is <mem>:<dtype>:<shape>, got '" + spec + "'");
  Tensor t; t.memory = c.r.as<MemoryID>(parts[0]);
  if (!parse_dtype(parts[1], t.dtype)) fail(Status::Invalid, "bad dtype in '" + spec + "'");
  t.shape = parse_shape(parts[2]);
  return t;
}

void print_report(Ctx& c, const ExecutionReport& rep) {
  c.o << "execution " << rep.graph.short_str() << ": ran " << rep.operations_run << "/" << rep.operations_total << " ops, position " << rep.position << (rep.complete ? " (complete)" : " (resumable)") << " in " << fmt_ns(rep.elapsed_ns) << "\n";
  for (size_t i = 0; i < rep.samples.size(); ++i) {
    const auto& s = rep.samples[i]; const auto& p = rep.placements[i];
    c.o << "    wave " << p.wave << "  op " << s.op.short_str() << " → " << p.device_name << "(" << p.device.short_str() << ") lanes=" << s.lanes_used << " " << fmt_ns(s.finished_ns - s.started_ns) << "  [" << p.reason << (p.migrated_bytes ? ", migrated " + fmt_bytes(p.migrated_bytes) : "") << "]\n";
  }
  if (rep.preempted) c.o << "    PREEMPTED: the owner's execution budget ran out after " << fmt_ns(rep.charged_ns) << " of device time; resume from position " << rep.position << "\n";
  if (!rep.failure.empty()) c.o << "    FAILED: " << rep.failure << "\n";
}

// Execution quotas: what an entity is allowed to spend and what it has spent.
// The scheduler charges here, so what this prints is measured, not declared.
void cmd_quota(Ctx& c, const Args& a) {
  std::string sub = a.size() > 1 ? a[1] : "list";
  auto& q = c.k.quotas();
  if (sub == "list" || sub == "show") {
    c.view();
    c.o << c.o.style(Header()) << W{14} << "ENTITY" << W{18} << "LABEL" << W{12} << "SLICE"
        << W{12} << "BUDGET" << W{12} << "USED" << W{10} << "PERIOD" << W{8} << "STOPS" << "STATE"
        << c.o.style(Reset()) << "\n";
    for (const auto& e : q.list()) {
      const Entity* ent = c.k.entities().get(e.entity);
      c.o << W{14} << e.entity.short_str()
          << W{18} << (ent ? ent->label : std::string("(gone)"))
          << W{12} << fmt_ns(e.quota.timeslice_ns)
          << W{12} << fmt_ns(e.quota.budget_ns)
          << W{12} << fmt_ns(e.quota.used_ns)
          << W{10} << fmt_ns(e.quota.period_ns)
          << W{8}  << e.quota.preemptions
          << (e.quota.preempted ? "out of budget" : "runnable") << "\n";
    }
  } else if (sub == "set") {
    if (a.size() < 3) fail(Status::Invalid, "quota set <e> budget=<ns> [slice=<ns>] [period=<ns>]");
    need(c.me.operate());
    EntityID who = c.r.as<EntityID>(a[2]);
    uint64_t budget = has_kv(a, "budget") ? to_u64(kv(a, "budget")) : 0;
    uint64_t slice = has_kv(a, "slice") ? to_u64(kv(a, "slice")) : 0;
    uint64_t period = has_kv(a, "period") ? to_u64(kv(a, "period")) : 1000000000ull;
    if (budget == 0) fail(Status::Invalid, "quota set <e> budget=<ns> — a budget is required");
    need(q.assign_quota(who, slice, budget, period));
    c.o << "entity " << who.short_str() << " may spend " << fmt_ns(budget) << " of device time every "
        << fmt_ns(period) << ", in stretches of at most " << fmt_ns(slice ? slice : budget) << "\n";
  } else if (sub == "clear") {
    if (a.size() < 3) fail(Status::Invalid, "quota clear <e>");
    need(c.me.operate());
    need(q.clear_quota(c.r.as<EntityID>(a[2])));
    c.o << "quota cleared\n";
  } else if (sub == "reset") {
    need(c.me.operate());
    q.reset_period();
    c.o << "every accounting window reopened\n";
  } else {
    fail(Status::Invalid, "quota list | quota set <e> budget=<ns> [slice=<ns>] [period=<ns>] | quota clear <e> | quota reset");
  }
}

void cmd_graph(Ctx& c, const Args& a) {
  if (a.size() < 2) fail(Status::Invalid, "graph create|op|plan|partition|run|show");
  const std::string& sub = a[1];
  if (sub == "create") {
    if (a.size() < 3) fail(Status::Invalid, "graph create <e>");
    auto id = need(c.as(a[2]).create_graph());
    c.produced(id.str());
    c.o << "graph " << id.str() << "\n";
  } else if (sub == "op") {
    if (a.size() < 5) fail(Status::Invalid, "graph op <e> <g> <opcode> [in=<mem>:<dtype>:<shape>]... [out=...]... [scalar=x] [par=n] [prio=n] [dev=cpu] [after=<op>]");
    Operation op;
    if (!parse_opcode(a[4], op.code)) fail(Status::Invalid, "unknown opcode '" + a[4] + "'");
    for (size_t i = 5; i < a.size(); ++i) {
      const std::string& t = a[i];
      if (t.rfind("in=", 0) == 0) op.inputs.push_back(parse_tensor_ref(c, t.substr(3)));
      else if (t.rfind("out=", 0) == 0) op.outputs.push_back(parse_tensor_ref(c, t.substr(4)));
      else if (t.rfind("scalar=", 0) == 0) op.scalar = to_f64(t.substr(7));
      else if (t.rfind("par=", 0) == 0) op.requirement.parallelism = (uint32_t)to_u64(t.substr(4));
      else if (t.rfind("prio=", 0) == 0) op.requirement.priority = (uint32_t)to_u64(t.substr(5));
      else if (t.rfind("mem=", 0) == 0) op.requirement.memory = to_u64(t.substr(4));
      else if (t.rfind("dev=", 0) == 0) {
        std::string want = t.substr(4);
        if (!parse_device_type(want, op.requirement.device)) {
          // A name prefix (ocl-cpu, gpu:, remote:) or an id is kept AS WRITTEN and
          // resolved at placement, so the operation still names something real
          // after the graph moves to another kernel. Typos are caught here, now.
          bool answers = false;
          for (auto* d : c.k.devices().all()) if (d->name().rfind(want, 0) == 0 || d->id().short_str().rfind(want, 0) == 0) { answers = true; break; }
          if (!answers) fail(Status::NotFound, "no device answering to '" + want + "' in this kernel's fabric");
          op.requirement.device_selector = want;
        }
      }
      else if (t.rfind("after=", 0) == 0) op.depends.push_back(c.r.as<OperationID>(t.substr(6)));
      else if (t.rfind("kernel=", 0) == 0) op.custom_kernel = fnv1a(t.substr(7));
      else if (t.rfind("flops=", 0) == 0) { /* taken below, once the shapes are known */ }
      else fail(Status::Invalid, "unknown operation attribute '" + t + "'");
    }
    // The work the operation actually performs, from its opcode and shapes.
    // Counting input elements called a 64x64 matmul 8192 flops when it is
    // 2·64³, and every placement decision downstream believed it.
    if (has_kv(a, "flops")) op.requirement.flops = to_u64(kv(a, "flops"));
    else op.requirement.flops = intelligence::GraphAutoPartitioner::operation_flops(op);
    auto id = need(c.as(a[2]).add_operation(c.r.as<GraphID>(a[3]), op));
    c.produced(id.str());
    c.o << "operation " << id.str() << " (" << opcode_name(op.code) << ")\n";
  } else if (sub == "plan") {
    if (a.size() < 4) fail(Status::Invalid, "graph plan <e> <g>");
    auto plan = need(c.as(a[2]).plan(c.r.as<GraphID>(a[3])));
    for (auto& p : plan) c.o << "    op " << p.op.short_str() << " → " << p.device_name << " lanes=" << p.lanes << " [" << p.reason << "]\n";
  } else if (sub == "partition") {
    if (a.size() < 4) fail(Status::Invalid, "graph partition <e> <g> [link=<bytes/s>]");
    auto g = need(c.k.graphs().copy(c.r.as<GraphID>(a[3])));
    intelligence::PartitionCostModel model;
    if (has_kv(a, "link")) model.link_bytes_per_second = to_f64(kv(a, "link"));
    intelligence::GraphAutoPartitioner part(model);
    auto devs = c.k.devices().all();
    auto waves = need(part.partition_graph(g, devs));
    c.view();
    std::unordered_map<std::string, std::string> dev_name;
    for (auto* d : devs) dev_name[d->id().str()] = d->name();
    double critical_path = 0.0;
    for (auto& w : waves) {
      c.o << "  wave " << w.wave_index << "  " << w.operations.size() << " op(s), "
          << w.estimated_latency_ms << " ms on the slowest lane, busiest device "
          << dev_name[w.target_device.str()] << "\n";
      for (auto& p : w.placements) {
        const Operation* op = g.find(p.op);
        c.o << "      " << p.op.short_str() << "  " << W{11} << (op ? opcode_name(op->code) : "?")
            << " -> " << W{18} << dev_name[p.device.str()]
            << " compute " << p.compute_ms << " ms";
        if (p.transfer_ms > 0.0) c.o << ", transfer " << p.transfer_ms << " ms";
        if (op) c.o << "  (" << intelligence::GraphAutoPartitioner::operation_flops(*op) << " flop)";
        c.o << "\n";
      }
      critical_path += w.estimated_latency_ms;
    }
    c.o << "  " << waves.size() << " wave(s) over " << devs.size() << " device(s); critical path "
        << critical_path << " ms; graph cost " << part.estimate_cost(g)
        << " ms on the 1 GFLOP/s reference machine\n";
  } else if (sub == "run") {
    if (a.size() < 4) fail(Status::Invalid, "graph run <e> <g> [steps]");
    SchedulePolicy pol; if (a.size() > 4) pol.max_operations = to_u64(a[4]);
    auto rep = need(c.as(a[2]).execute(c.r.as<GraphID>(a[3]), pol));
    print_report(c, rep);
    if (!rep.failure.empty()) fail(Status::IoError, rep.failure);
  } else if (sub == "show") {
    if (a.size() < 3) fail(Status::Invalid, "graph show <g>");
    c.view();
    auto g = need(c.k.graphs().copy(c.r.as<GraphID>(a[2])));
    c.o << "graph " << g.id.str() << " owner=" << g.owner.short_str() << " ops=" << g.operations.size() << " flops≈" << g.total_flops() << " footprint=" << fmt_bytes(g.footprint()) << "\n";
    for (auto& op : g.operations) {
      // The requirement as written, not a device instance: this is what travels.
      std::string dev = !op.requirement.device_selector.empty() ? op.requirement.device_selector
                      : (!op.requirement.affinity.is_null() ? op.requirement.affinity.short_str() : device_type_name(op.requirement.device));
      c.o << "    " << op.id.short_str() << "  " << W{11} << opcode_name(op.code) << " dev=" << dev << " par=" << op.requirement.parallelism << " prio=" << op.requirement.priority;
      for (auto& t : op.inputs) c.o << "  in " << t.describe();
      for (auto& t : op.outputs) c.o << "  out " << t.describe();
      if (op.code == OpCode::Fill || op.code == OpCode::Scale) c.o << "  scalar=" << op.scalar;
      for (auto& d : op.depends) c.o << "  after " << d.short_str();
      c.o << "\n";
    }
    auto order = g.order();
    if (order.ok()) { c.o << "    order:"; for (auto& o : *order) c.o << " " << o.short_str(); c.o << "\n"; }
    else c.o << "    order: " << order.error().str() << "\n";
  } else fail(Status::Invalid, "unknown: graph " + sub);
}

// The compute service: programs that are not aios ask this kernel for work,
// one JSON request at a time, and each request runs as a graph through the
// scheduler. `compute kernels` only registers the ERP kernels on the CPU
// devices, for a graph built by hand with kernel=erp.tsp_exact.
namespace {
std::atomic<bool> g_compute_stop{false};
void compute_stop_signal(int) { g_compute_stop.store(true); }
}  // namespace

void cmd_compute(Ctx& c, const Args& a) {
  if (a.size() < 2) fail(Status::Invalid, "compute kernels | compute serve [port=7311] [lanes=N] [max_points=15] [token=T] [public=1] [sql=1] [sql_db=sql_db] [cdc_host=H cdc_port=P cdc_user=U cdc_pass=PW cdc_db=DB cdc_tables=t1,t2,... cdc_log_file=F cdc_log_pos=N]");
  const std::string& sub = a[1];
  auto register_all = [&]() -> size_t {
    size_t n = 0;
    for (auto* d : c.k.devices().of_type(DeviceType::CPU)) { need(erp::register_erp_kernels(*d)); ++n; }
    return n;
  };
  if (sub == "kernels") {
    size_t n = register_all();
    c.o << erp::kTspExactKernel << ", " << erp::kRouteImproveKernel << ", " << erp::kPoolGreedyKernel << " registered on " << n << " cpu device(s)\n";
    return;
  }
  if (sub == "serve") {
    userland::install_crash_handler();   // a fatal signal is logged (signal, address, last statement, backtrace) before the engine dies
    channel::ComputeServeOptions o;
    if (has_kv(a, "port")) o.port = (uint16_t)to_u64(kv(a, "port"));
    if (has_kv(a, "lanes")) o.lanes = (uint32_t)to_u64(kv(a, "lanes"));
    if (has_kv(a, "max_points")) o.max_exact_points = (int)to_u64(kv(a, "max_points"));
    // shm_ring=NAME: also serve requests from the /dev/shm ring pair NAME and
    // NAME_resp (compute_wire.hpp) for processes on this machine.
    if (has_kv(a, "shm_ring")) o.shm_ring_name = kv(a, "shm_ring");
    if (has_kv(a, "token")) o.token = c.r.expand(kv(a, "token"));
    // A token in the unit file is readable by anyone who can run
    // `systemctl cat`; a file the service user owns is not.
    if (has_kv(a, "token_file")) {
      if (read_first_line(kv(a, "token_file"), o.token)) {
      } else {
        c.o << "compute serve: cannot read " << kv(a, "token_file") << "\n";
      }
    }
    if (has_kv(a, "public")) o.loopback_only = kv(a, "public") != "1";
    size_t n = register_all();
    // The service is a RESIDENT of this root, not a spawn of this process: the
    // same identity every boot, so a record's provenance still names an entity
    // a later process can show. Its authority is what the work needs and no
    // more — schedule, execute, a bounded memory budget, and the store.
    using K = ResourceKind;
    AuthorityProfile profile = {
      {ResourceRef{K::Kernel, {}},        (uint16_t)R_Schedule, 0},
      {ResourceRef{K::ComputeDevice, {}}, (uint16_t)R_Execute, 0},
      {ResourceRef{K::Memory, {}},        (uint16_t)(R_Read | R_Write | R_Allocate | R_Destroy), 1ull << 30},
      {ResourceRef{K::State, {}},         (uint16_t)(R_Read | R_Write | R_Allocate | R_Destroy), 0},
      {ResourceRef{K::StorageObject, {}}, (uint16_t)(R_Read | R_Write), 0},
    };
    EntityID e = need(c.me.resident("erp-compute", profile));   // the console's authority, checked by the kernel
    Surface w = need(c.me.act_as(e));
    hal::Mutex log_mu;
    auto log = [&](const std::string& line) { hal::LockGuard g(log_mu); c.o << line << "\n"; };
    // Opt-in: the "sql" op only exists when this instance is asked for it, and
    // even then only ever answers reads (see compute_serve's is_read_only_sql)
    // off the same on-disk tables `aios sql query` reads — no separate copy,
    // no separate import step, just a second door to what is already there.
    std::unique_ptr<userland::TaskLedger> sql_ledger;
    std::unique_ptr<userland::SqlDatabase> sql_db;
    std::unique_ptr<userland::MysqlWireServer> mysql_server;
    std::unique_ptr<sidecar::CapabilityGuard> asql_cap_guard;
    std::unique_ptr<userland::AsqlUserStore> asql_cap_users;
    bool mysql_wire_up = false;
    if (has_kv(a, "sql") && kv(a, "sql") == "1") {
      sql_ledger = std::make_unique<userland::TaskLedger>(c.k);
      sql_ledger->init("tasks");
      sql_db = std::make_unique<userland::SqlDatabase>(c.k, sql_ledger.get());
      const std::string db_dir = has_kv(a, "sql_db") ? kv(a, "sql_db") : std::string("sql_db");
      // The source schema the whitelist lives in. Declared here, not inside the
      // cdc_host block, because the whitelist seed below needs it after that
      // block closes. There is no default: a mirror names what it mirrors.
      const std::string cdc_db_seed = has_kv(a, "cdc_db") ? kv(a, "cdc_db") : std::string();
      if (has_kv(a, "cdc_host") && cdc_db_seed.empty())
        fail(Status::Invalid, "compute serve: cdc_host= needs cdc_db=<the source database to mirror>");
      // The MySQL wire server (Navicat, DBeaver, the mysql client, the ERP), started before anything loads.
      uint16_t mysql_port = has_kv(a, "mysql_port") ? (uint16_t)to_u64(kv(a, "mysql_port")) : (has_kv(a, "port_mysql") ? (uint16_t)to_u64(kv(a, "port_mysql")) : 3307);
      std::string mysql_bind = has_kv(a, "mysql_bind") ? kv(a, "mysql_bind") : (has_kv(a, "public") && kv(a, "public") == "1" ? "0.0.0.0" : "0.0.0.0");
      if (has_kv(a, "mysql_loopback") && kv(a, "mysql_loopback") == "1") mysql_bind = "127.0.0.1";

      std::string mysql_user = has_kv(a, "mysql_user") ? kv(a, "mysql_user") : std::string();
      std::string mysql_pass = has_kv(a, "mysql_pass") ? kv(a, "mysql_pass") : std::string();
      // A password in the unit file shows up in `systemctl cat` and in `ps`;
      // a file readable only by the service user does not.
      if (has_kv(a, "mysql_pass_file")) {
        if (read_first_line(kv(a, "mysql_pass_file"), mysql_pass)) {
        } else {
          c.o << "compute mysql wire server: cannot read " << kv(a, "mysql_pass_file") << "\n";
        }
      }

      // Without an account this port accepted any credentials at all. It is
      // now kept on the loopback until one is configured, rather than opened
      // to the internet unauthenticated.
      if (mysql_user.empty() && mysql_bind != "127.0.0.1") {
        c.o << "compute mysql wire server: no mysql_user set, binding 127.0.0.1 only"
               " (set mysql_user= and mysql_pass_file= to accept remote clients)\n";
        mysql_bind = "127.0.0.1";
      }

      mysql_server = std::make_unique<userland::MysqlWireServer>(c.k, *sql_db, mysql_port, mysql_bind);
      if (!mysql_user.empty()) mysql_server->add_account(mysql_user, mysql_pass);
      mysql_server->set_trust_loopback(!has_kv(a, "mysql_strict_loopback") ||
                                       kv(a, "mysql_strict_loopback") != "1");
      // Capability credentials (user "cap"): signed with this machine's
      // capability key, revocations kept beside it. mysql_capabilities=0 turns
      // them off; mysql_cap_remote=1 accepts them from other hosts, in the clear.
      if (!has_kv(a, "mysql_capabilities") || kv(a, "mysql_capabilities") != "0") {
        const std::string cap_root = c.k.config().root;
        const std::string cap_key = userland::asql_load_capability_key(cap_root);
        if (cap_key.empty()) {
          c.o << "compute mysql wire server: no capability key (set AIOS_CAPABILITY_KEY or make " << cap_root
              << "/capability.key writable); capability logins are off\n";
        } else {
          asql_cap_guard = std::make_unique<sidecar::CapabilityGuard>(cap_key, cap_root + "/capability-state");
          mysql_server->set_capability_guard(asql_cap_guard.get());
          asql_cap_users = std::make_unique<userland::AsqlUserStore>(cap_root + "/capability-users");
          mysql_server->set_capability_users(asql_cap_users.get());
          mysql_server->set_capability_remote(has_kv(a, "mysql_cap_remote") && kv(a, "mysql_cap_remote") == "1");
          c.o << "compute mysql wire server: capability logins on (user cap"
              << (has_kv(a, "mysql_cap_remote") && kv(a, "mysql_cap_remote") == "1" ? ", remote allowed" : ", this host only")
              << "; named users in " << cap_root << "/capability-users)\n";
        }
      }
      if (has_kv(a, "mysql_socket")) mysql_server->set_unix_socket(kv(a, "mysql_socket"));
      if (has_kv(a, "mysql_charset")) mysql_server->set_charset_aware(kv(a, "mysql_charset") == "1");
      if (has_kv(a, "max_connections")) mysql_server->set_max_connections(to_u64(kv(a, "max_connections")));
      if (has_kv(a, "query_memory_mb")) sql_db->set_query_memory_budget((size_t)to_u64(kv(a, "query_memory_mb")) << 20);
      // TLS: mysql_tls_cert=<chain.pem> mysql_tls_key=<key.pem> [mysql_tls_require=1]
      if (has_kv(a, "mysql_tls_cert") && has_kv(a, "mysql_tls_key")) {
        mysql_server->set_tls(kv(a, "mysql_tls_cert"), kv(a, "mysql_tls_key"), has_kv(a, "mysql_tls_require") && kv(a, "mysql_tls_require") == "1");
        c.o << "compute mysql wire server: TLS offered (" << kv(a, "mysql_tls_cert") << ")"
            << (has_kv(a, "mysql_tls_require") && kv(a, "mysql_tls_require") == "1" ? ", required for remote clients" : "") << "\n";
      }
      // Listening from here on, before the store loads: a client logs in and is told what the engine is doing
      // (MysqlWireServer::set_starting) until it is up.
      mysql_server->set_starting(true);
      auto ms_res = mysql_server->start();
      if (ms_res.ok()) {
        mysql_server->start_background();
        mysql_wire_up = true;
        if (!mysql_server->unix_socket().empty())
          c.o << "compute mysql wire server listening on unix socket " << mysql_server->unix_socket() << "\n";
        c.o << "compute mysql wire server listening on " << mysql_bind << ":" << mysql_port
            << (mysql_user.empty() ? " (loopback, no password)" : " (password required for user " + mysql_user + ")")
            << "\n";
      } else {
        c.o << "compute mysql wire server failed to bind " << mysql_bind << ":" << mysql_port << ": " << ms_res.error().message << "\n";
      }
      sql_db->set_startup_phase("opening the store");
      need(sql_db->init(db_dir));
      sql_db->scan_local_stores();  // discover databases dropped into the root (not just sql_db/)
      c.o << "compute sql op attached: " << db_dir << " (" << sql_db->list_tables().size() << " table(s), reads only)\n";
      // This engine is the machine's: fleet peers' sql_on() reaches it (read-
      // only, like the wire) rather than a second engine opened on the same
      // store that would only ever show the data as it was at startup.
      userland::set_machine_sql(c.k, sql_db.get(), true);
      // The sync whitelist: `cdc_tables=@file` (or a path), one table per
      // line, # comments; or the tables themselves, `cdc_tables=t1,t2`, as
      // the usage line says. A bare list used to be read as a file name,
      // found no file, and left the whitelist empty - which mirrors every
      // table the binlog mentions - without a word.
      std::vector<std::string> cdc_want;
      bool cdc_whitelist_bad = false;
      if (has_kv(a, "cdc_tables")) {
        std::string wl = kv(a, "cdc_tables");
        const bool is_file = (!wl.empty() && wl[0] == '@') || wl.find('/') != std::string::npos;
        auto add_table = [&](std::string tbl) {
          while (!tbl.empty() && (tbl.back() == '\r' || tbl.back() == ' ' || tbl.back() == '\t')) tbl.pop_back();
          size_t s = tbl.find_first_not_of(" \t");
          if (s == std::string::npos) return;
          tbl = tbl.substr(s);
          if (!tbl.empty() && tbl[0] == '#') return;
          cdc_want.push_back(tbl);
        };
        if (is_file) {
          std::string wl_path = wl[0] == '@' ? wl.substr(1) : wl;
          std::vector<std::string> wl_lines;
          if (read_file_lines(wl_path, wl_lines)) {
            for (const std::string& tbl : wl_lines) add_table(tbl);
          } else {
            c.o << "compute cdc: cannot read the cdc_tables file " << wl_path << "; not streaming (an empty whitelist would mirror every table)\n";
            cdc_whitelist_bad = true;
          }
        } else {
          size_t from = 0;
          while (from <= wl.size()) {
            const size_t comma = wl.find(',', from);
            add_table(wl.substr(from, comma == std::string::npos ? std::string::npos : comma - from));
            if (comma == std::string::npos) break;
            from = comma + 1;
          }
        }
      }
      // Auto-start CDC binlog stream when cdc_host is given
      // A replication account is the operator's to name; guessing one would
      // stream as whoever happens to exist on the source.
      if (has_kv(a, "cdc_host") && !cdc_whitelist_bad && !has_kv(a, "cdc_user")) {
        c.o << "compute cdc: cdc_host is set but cdc_user is not; not streaming\n";
        cdc_whitelist_bad = true;
      }
      if (has_kv(a, "cdc_host") && !cdc_whitelist_bad) {
        const std::string cdc_host = kv(a, "cdc_host");
        const uint16_t cdc_port = has_kv(a, "cdc_port") ? (uint16_t)to_u64(kv(a, "cdc_port")) : 3306;
        const std::string cdc_user = kv(a, "cdc_user");
        std::string cdc_pass = has_kv(a, "cdc_pass") ? kv(a, "cdc_pass") : std::string();
        // The replication password in the unit file is readable by anyone who
        // can run `systemctl cat`, and shows in `ps` for the whole lifetime of
        // the process. A file the service user owns is not.
        if (has_kv(a, "cdc_pass_file")) {
          if (read_first_line(kv(a, "cdc_pass_file"), cdc_pass)) {
          } else {
            c.o << "compute cdc: cannot read " << kv(a, "cdc_pass_file") << "\n";
          }
        }
        // A binlog stream carries row deltas, so a whitelisted table that was
        // never copied in stays invisible however long the engine streams.
        // Copy the missing ones from the source FIRST - one consistent
        // snapshot, its binlog position remembered per table - and only then
        // start the stream, which picks each table up where its copy ends.
        // (This used to run after the stream started, and "copied" by running
        // the SELECT against this engine's own store: it only ever found
        // tables already there.)
        if (!cdc_want.empty()) {
          std::vector<std::string> seed_log;
          sql_db->set_mirror_source(userland::SqlDatabase::MirrorSource{cdc_host, cdc_port, cdc_user, cdc_pass, cdc_db_seed, cdc_want});
          auto sd = sql_db->seed_from_source(cdc_host, cdc_port, cdc_user, cdc_pass, cdc_db_seed, cdc_want, &seed_log);
          for (const auto& line : seed_log) log("compute sql: seed " + line);
          if (!sd.ok()) log("compute sql: seed failed: " + sd.error().message);
          size_t have = 0;
          for (const auto& t : cdc_want) if (sql_db->find_table(t, cdc_db_seed) || sql_db->find_view(t, cdc_db_seed)) ++have;
          log("compute sql: whitelist - " + std::to_string(have) + " of " + std::to_string(cdc_want.size()) + " table(s) present" +
              (sd.ok() ? ", " + std::to_string(*sd) + " seeded now" : std::string()));
          // Tables already here: their column metadata re-read from the master.
          sql_db->set_startup_phase("re-reading table definitions from the master");
          std::vector<std::string> rep_log;
          auto rp = sql_db->repair_schema_from_source(cdc_host, cdc_port, cdc_user, cdc_pass, cdc_db_seed, cdc_want, &rep_log);
          for (const auto& l : rep_log) log("compute sql: schema - " + l);
          if (!rp.ok()) log("compute sql: schema repair skipped: " + rp.error().message);
          else if (*rp) log("compute sql: schema - " + std::to_string(*rp) + " table(s) re-typed from the master");
        }
        // An explicit cdc_log_file/cdc_log_pos pins a position (a snapshot
        // taken outside, e.g. `mysqldump --single-transaction --master-data=2`).
        // Otherwise the saved position is used - which a seed writes if there
        // was none - and only a store that has never streamed or seeded falls
        // back to the source's current position.
        std::string change = "CHANGE REPLICATION SOURCE TO SOURCE_HOST = '" + cdc_host +
            "', SOURCE_PORT = " + std::to_string(cdc_port) +
            ", SOURCE_USER = '" + cdc_user +
            "', SOURCE_PASSWORD = '" + cdc_pass +
            "', SOURCE_DATABASE = '" + cdc_db_seed + "'";
        if (has_kv(a, "cdc_log_file")) {
          change += ", SOURCE_LOG_FILE = '" + kv(a, "cdc_log_file") + "'";
        }
        if (has_kv(a, "cdc_log_pos")) {
          change += ", SOURCE_LOG_POS = " + kv(a, "cdc_log_pos");
        }
        change += ";";
        sql_db->set_startup_phase("starting replication");
        auto cr = sql_db->execute_query(change);
        if (cr.success) {
          // The whitelist as the table names read above. It used to pass the
          // `@/path/file` text itself, which the SQL tokenizer split into the
          // path's words - a table was followed only if its name happened to
          // be one of them (probe, in /tmp/asql-probe.tables); on st, whose
          // file is /etc/aios/whitelist.tables, none of the six ever were.
          if (!cdc_want.empty()) {
            std::string names;
            for (const auto& t : cdc_want) {
              std::string q = "`";
              for (char ch : t) { if (ch == '`') q += "``"; else q += ch; }
              names += (names.empty() ? "" : ", ") + q + "`";
            }
            sql_db->execute_query("SET REPLICATION WHITELIST = " + names + ";");
          }
          auto sr = sql_db->execute_query("START REPLICA;");
          c.o << "compute cdc: " << (sr.success ? "streaming from " + cdc_host + ":" + std::to_string(cdc_port) : "failed: " + sr.error_message) << "\n";
        } else {
          c.o << "compute cdc config failed: " << cr.error_message << "\n";
        }
      }

      if (has_kv(a, "tables_dir")) {
        // Where <database>.tables lists are published for same-host clients (the ERP
        // reads them in place of SHOW TABLES). Each is rewritten after the CDC apply
        // that changed that database's tables.
        sql_db->set_table_list_dir(kv(a, "tables_dir"));
        sql_db->publish_table_lists();
        c.o << "compute table lists published to " << kv(a, "tables_dir") << "\n";
      }
      if (mysql_wire_up) {
        // Advertise this node's databases in the fleet telemetry so peers can
        // find them without any static connections.ini configuration. Read-only
        // here means "a peer's write would get overwritten by the next CDC
        // tick", not "the wire protocol rejects writes" -- it never does, on
        // any table. A standalone store with no cdc_host has nothing to
        // overwrite it, so it is the fleet's honest primary for this data.
        sql_db->sync_telemetry_registry(o.port, mysql_port, /*read_only=*/has_kv(a, "cdc_host"));
        mysql_server->set_starting(false);
        c.o << "compute mysql wire server: answering queries\n";
      }
      sql_db->set_startup_phase("ready");
      // grain ps: this engine, its ports and its Things (runtime_registry.hpp)
      userland::runtime::announce("asql", "compute serve sql=1", c.k.config().root);
      if (mysql_wire_up) userland::runtime::add_port(mysql_port, "mysql");
      userland::runtime::add_port(o.port, "rpc");
      userland::SqlDatabase* things_db = sql_db.get();
      userland::runtime::keep_current("things", [things_db] {
        auto r = things_db->execute_gateway_query("SHOW THINGS");
        if (!r.success) return std::string("[]");
        auto js = [](const std::string& s) {
          std::string q = "\"";
          for (unsigned char ch : s) {
            if (ch == '"' || ch == '\\') { q += '\\'; q += (char)ch; }
            else if (ch >= 0x20) q += (char)ch;
          }
          return q + "\"";
        };
        static const char* keep[] = {"thing", "things", "version", "reactions", "full_recomputes", "state", "declared"};
        std::string out = "[";
        for (size_t i = 0; i < r.rows.size(); ++i) {
          out += i ? ",{" : "{";
          bool first = true;
          for (size_t k = 0; k < r.columns.size() && k < r.rows[i].values.size(); ++k) {
            if (std::find(std::begin(keep), std::end(keep), r.columns[k]) == std::end(keep)) continue;
            out += (first ? "" : ",") + js(r.columns[k]) + ":" + js(r.rows[i].values[k]);
            first = false;
          }
          out += "}";
        }
        return out + "]";
      }, 5);
    }
    // The rpc "sql" op (see compute_serve's is_read_only_sql) is hardcoded
    // read-only and always will be; the mysql wire server started just above,
    // when running, accepts writes on every table regardless. Naming both
    // here, instead of one bare "sql: read-only", is what stops an operator
    // reading this banner from assuming the wire server is equally
    // restricted -- it never was.
    std::string sql_status;
    if (sql_db) {
      sql_status = "; sql op: read-only";
      if (mysql_wire_up) {
        // Read-write either way: the wire server rejects no write. On a cdc
        // mirror a local write lasts until replication rewrites that row,
        // which is what the fleet's read_only flag means - not a refusal.
        sql_status += has_kv(a, "cdc_host")
            ? "; mysql wire: read-write (cdc mirror: replication overwrites local writes)"
            : "; mysql wire: read-write";
      }
    }
    c.o << "compute service as entity " << e.short_str() << " over " << n << " cpu device(s); kernels: " << erp::kTspExactKernel << ", " << erp::kRouteImproveKernel << ", " << erp::kPoolGreedyKernel << sql_status << "\n";
    // SIGTERM (systemd stop, a deploy) ends the service by writing the store out first, so the next start finds
    // no redo log to replay: the records of the last few minutes took a second and a half to replay on chi-il-1,
    // on every restart.
    g_compute_stop.store(false);
    std::signal(SIGTERM, compute_stop_signal);
    std::signal(SIGINT, compute_stop_signal);
    need(channel::compute_serve(c.k, w, o, log, &g_compute_stop, sql_db.get()));
    userland::runtime::stop_keeping("things");   // the engine is about to go
    if (sql_db && g_compute_stop.load()) {
      userland::runtime::withdraw();   // _Exit below runs no exit handlers
      const uint64_t t0 = c.k.host().monotonic_ns();
      auto flushed = sql_db->flush_for_exit();
      c.o << "compute sql: " << (flushed.ok() ? "store written" : "store NOT written (" + flushed.error().message + ")") << " in "
          << fmt_double((double)(c.k.host().monotonic_ns() - t0) / 1e9, 6, true, 2) << " s; stopping\n";
      std::cout.flush();
      std::_Exit(flushed.ok() ? 0 : 1);   // the threads the engine started are not waited for one by one
    }
    return;
  }
  fail(Status::Invalid, "unknown: compute " + sub);
}

// The in-tree Qwen2 runtime: weights from a safetensors checkpoint (plus an
// optional LoRA adapter), run on this machine's lanes, answering chat requests
// directly or as an OpenAI-compatible endpoint.
userland::LlmModel* infer_load(Ctx& c, const Args& a) {
  std::string dir = kv(a, "model");
  if (dir.empty()) dir = hal::process_host().env("AIOS_LLM_MODEL");
  if (dir.empty()) dir = c.k.config().root + "/models/default";
  std::string adapter = kv(a, "adapter");
  if (adapter.empty() && !has_kv(a, "adapter")) adapter = hal::process_host().env("AIOS_LLM_ADAPTER");
  const uint32_t context = has_kv(a, "context") ? (uint32_t)to_u64(kv(a, "context")) : 4096;
  const uint64_t t0 = c.k.host().monotonic_ns();
  auto* m = need(userland::resident_llm_load(c.k.host(), dir, adapter, context));
  const auto& cfg = m->config();
  c.o << "loaded " << userland::resident_llm_source() << ": " << cfg.layers << " layers, hidden "
      << cfg.hidden << ", " << cfg.heads << "/" << cfg.kv_heads << " heads, vocab " << cfg.vocab << ", context "
      << m->context() << ", " << fmt_bytes(m->weight_bytes()) << " of weights, "
      << fmt_double((double)(c.k.host().monotonic_ns() - t0) / 1e9, 6, true, 2) << " s\n";
  return m;
}

// The loaded model, or the one load would pick when none is loaded yet.
userland::LlmModel* infer_model(Ctx& c, const Args& a) {
  if (auto* m = userland::resident_llm_loaded()) {
    if (!has_kv(a, "model") && !has_kv(a, "adapter")) return m;
  }
  return infer_load(c, a);
}

void cmd_infer(Ctx& c, const Args& a) {
  const std::string sub = a.size() > 1 ? a[1] : "status";
  if (sub == "status") {
    auto* m = userland::resident_llm_loaded();
    if (!m) { c.o << "no model loaded: infer load model=<dir> [adapter=<dir>] [context=N]\n"; return; }
    const auto& cfg = m->config();
    c.o << "model    : " << userland::resident_llm_source() << "\n"
        << "shape    : " << cfg.layers << " layers, hidden " << cfg.hidden << ", " << cfg.heads << " heads (" << cfg.kv_heads
        << " kv), ffn " << cfg.ffn << ", vocab " << cfg.vocab << "\n"
        << "context  : " << m->context() << " tokens\n"
        << "weights  : " << fmt_bytes(m->weight_bytes()) << "\n"
        << "system   : " << m->default_system() << "\n";
    return;
  }
  if (sub == "load") { infer_load(c, a); return; }
  if (sub == "ask") {
    std::string text;
    auto* m = infer_model(c, a);
    // The checkpoint's own sampling settings, decoded greedily unless asked otherwise.
    userland::SampleParams sp = m->defaults();
    sp.temperature = has_kv(a, "temperature") ? (float)std::stod(kv(a, "temperature")) : 0.0f;
    if (has_kv(a, "top_p")) sp.top_p = (float)std::stod(kv(a, "top_p"));
    sp.max_tokens = has_kv(a, "max_tokens") ? (uint32_t)to_u64(kv(a, "max_tokens")) : 128;
    if (has_kv(a, "seed")) sp.seed = to_u64(kv(a, "seed"));
    for (size_t i = 2; i < a.size(); ++i) {
      if (a[i].find('=') != std::string::npos && (a[i].rfind("temperature=", 0) == 0 || a[i].rfind("top_p=", 0) == 0 ||
          a[i].rfind("max_tokens=", 0) == 0 || a[i].rfind("seed=", 0) == 0 || a[i].rfind("model=", 0) == 0 ||
          a[i].rfind("adapter=", 0) == 0 || a[i].rfind("context=", 0) == 0)) continue;
      if (!text.empty()) text += ' ';
      text += a[i];
    }
    if (text.empty()) fail(Status::Invalid, "infer ask <text> [temperature=T] [top_p=P] [max_tokens=N] [seed=S]");
    auto r = need(m->generate(c.k.host(), m->chat_prompt({{"user", text}}), sp));
    c.o << r.text << "\n";
    c.o << "(" << r.prompt_tokens << " prompt tokens in " << fmt_double(r.prefill_ms, 6, true, 0) << " ms, " << r.completion_tokens
        << " generated in " << fmt_double(r.decode_ms, 6, true, 0) << " ms"
        << (r.completion_tokens ? ", " + fmt_double(r.completion_tokens / (r.decode_ms / 1e3), 6, true, 2) + " tok/s" : std::string())
        << ")\n";
    return;
  }
  if (sub == "serve") {
    auto* m = infer_model(c, a);
    channel::LlmServeOptions o;
    if (has_kv(a, "port")) o.port = (uint16_t)to_u64(kv(a, "port"));
    if (has_kv(a, "name")) o.model_name = kv(a, "name");
    if (has_kv(a, "public")) o.loopback_only = kv(a, "public") != "1";
    if (has_kv(a, "key_file")) {
      if (!read_first_line(kv(a, "key_file"), o.api_key)) fail(Status::NotFound, "infer serve: cannot read " + kv(a, "key_file"));
    } else {
      o.api_key = hal::process_host().env("AIOS_LLM_KEY");
    }
    auto log = [&c](const std::string& line) { c.o << line << "\n"; c.o.flush(); };
    need(channel::llm_serve(*m, c.k.host(), o, log, nullptr));
    return;
  }
  fail(Status::Invalid, "infer load|status|ask|serve  (load model=<dir> [adapter=<dir>] [context=N]; ask <text>; serve [port=8000] [name=spark] [public=1 key_file=F])");
}

void cmd_channel(Ctx& c, const Args& a) {
  if (a.size() < 2) fail(Status::Invalid, "channel create|find|admit|subscribe|publish|consume|list");
  const std::string& sub = a[1];
  if (sub == "create") {
    if (a.size() < 5) fail(Status::Invalid, "channel create <e> <capacity> <slot_bytes> [transport=network:<addr>]");
    std::string transport = c.r.expand(kv(a, "transport", "shared-memory"));
    auto id = need(c.as(a[2]).create_channel((uint32_t)to_u64(a[3]), to_u64(a[4]), transport));
    c.produced(id.str());
    const Channel* ch = c.k.channels().get(id);
    c.o << "channel " << id.str() << " (" << ch->capacity << " frames × " << fmt_bytes(ch->slot_bytes) << ", transport " << ch->transport << ", region " << ch->region.short_str() << ")\n";
  } else if (sub == "find") {
    if (a.size() < 3) fail(Status::Invalid, "channel find <owner>");
    EntityID owner = c.r.as<EntityID>(a[2]);
    const Channel* hit = nullptr;
    for (auto& ch : c.k.channels().list()) if (ch.owner == owner && (!hit || ch.created_ns >= hit->created_ns)) hit = c.k.channels().get(ch.id);
    if (!hit) fail(Status::NotFound, "no channel owned by " + owner.short_str());
    c.produced(hit->id.str());
    c.o << "channel " << hit->id.str() << "\n";
  } else if (sub == "admit") {
    if (a.size() < 6) fail(Status::Invalid, "channel admit <e> <ch> <to> <rights>");
    uint16_t rights; if (!parse_rights(a[5], rights)) fail(Status::Invalid, "bad rights");
    auto id = need(c.as(a[2]).admit(c.r.as<ChannelID>(a[3]), c.r.as<EntityID>(a[4]), rights));
    c.produced(id.str());
    c.o << "capability " << id.str() << " (" << rights_str(rights) << " over channel)\n";
  } else if (sub == "subscribe") {
    if (a.size() < 4) fail(Status::Invalid, "channel subscribe <e> <ch>");
    need(c.as(a[2]).subscribe(c.r.as<ChannelID>(a[3])));
    c.o << "subscribed\n";
  } else if (sub == "publish") {
    if (a.size() < 5) fail(Status::Invalid, "channel publish <e> <ch> <state>");
    auto seq = need(c.as(a[2]).publish(c.r.as<ChannelID>(a[3]), c.r.as<StateID>(a[4])));
    c.o << "published frame #" << seq << "\n";
  } else if (sub == "consume") {
    if (a.size() < 4) fail(Status::Invalid, "channel consume <e> <ch>");
    auto f = need(c.as(a[2]).consume(c.r.as<ChannelID>(a[3])));
    c.o << "frame #" << f.sequence << " from " << f.publisher.short_str() << " state " << f.state.short_str() << " (" << state_kind_name(f.kind) << " gen=" << f.generation << ", " << f.memory.size() << " memory refs)  " << c.bytes_line(f.payload) << "\n";
    c.s.vars["read"] = std::string(f.payload.begin(), f.payload.end());
  } else if (sub == "list") {
    c.view();
    for (auto& ch : c.k.channels().list()) {
      uint64_t head = 0, tail = 0, dropped = 0;
      c.k.channels().stats(ch.id, head, tail, dropped);
      c.o << ch.id.short_str() << "  owner=" << ch.owner.short_str() << " capacity=" << ch.capacity << " slot=" << fmt_bytes(ch.slot_bytes) << " transport=" << ch.transport << " head=" << head << " tail=" << tail << " dropped=" << dropped << " subscribers=" << ch.cursors.size() << "\n";
    }
  } else fail(Status::Invalid, "unknown: channel " + sub);
}

void cmd_checkpoint(Ctx& c, const Args& a) {
  if (a.size() < 2) fail(Status::Invalid, "checkpoint <e>");
  EntityID e = c.r.as<EntityID>(a[1]);
  auto path = need(c.me.checkpoint_of(e));
  c.produced(path);
  c.s.vars["path"] = path;
  c.o << "checkpoint " << path << "\n";
}

void cmd_rewind(Ctx& c, const Args& a) {
  if (a.size() < 2) fail(Status::Invalid, "rewind <path>");
  std::string path = c.r.expand(a[1]);
  auto id = need(c.me.rewind(path));
  c.produced(id.str());
  auto e = need(c.k.entities().copy(id));
  c.o << "rewound entity " << id.str() << " (" << e.label << ") to " << path
      << " - states=" << e.states.size() << " memory=" << e.memory.size()
      << " caps=" << e.capabilities.size() << " restores=" << e.restores;
  if (!e.execution.is_null()) c.o << " execution=" << e.execution.short_str() << "@" << e.execution_position;
  c.o << "\n";
}

void cmd_restore(Ctx& c, const Args& a) {
  if (a.size() < 2) fail(Status::Invalid, "restore <path>");
  std::string path = c.r.expand(a[1]);
  auto id = need(c.me.restore(path));
  c.produced(id.str());
  auto e = need(c.k.entities().copy(id));
  c.o << "restored entity " << id.str() << " (" << e.label << ") states=" << e.states.size() << " memory=" << e.memory.size() << " caps=" << e.capabilities.size() << " restores=" << e.restores;
  if (!e.execution.is_null()) c.o << " execution=" << e.execution.short_str() << "@" << e.execution_position;
  c.o << "\n";
}

#if !defined(AIOS_METAL)
// AOS_PROVIDER / AOS_MODEL in the machine's own settings file, replaced in
// place. This is the file the bridge reads to decide which model the machine
// thinks with, so writing it is what "change the model" means.
void set_machine_conf(Kernel& k, const std::string& key, const std::string& value) {
  std::string path;
  if (const char* c = std::getenv("AOS_CONF")) path = c;
  if (path.empty()) {
    const char* home = std::getenv("HOME");
    path = std::string(home ? home : ".") + "/.config/aos.conf";
  }
  std::string body;
  if (auto cur = k.host().read_file(path); cur.ok())   // host-io: the machine's settings file (aos.conf)
    body.assign((const char*)cur->data(), cur->size());
  std::string out;
  bool replaced = false;
  size_t at = 0;
  while (at < body.size()) {
    size_t nl = body.find('\n', at);
    std::string line = body.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
    at = (nl == std::string::npos) ? body.size() : nl + 1;
    if (line.rfind(key + "=", 0) == 0) { out += key + "=" + value + "\n"; replaced = true; }
    else if (!line.empty()) out += line + "\n";
  }
  if (!replaced) out += key + "=" + value + "\n";
  (void)k.host().write_file(path, ByteSpan((const uint8_t*)out.data(), out.size()));   // host-io: the machine's settings file (aos.conf)
}
#endif

// The resident entity a mind is bound to, by label.
EntityID resident_mind_entity(Kernel& k, const std::string& label = "mind") {
  for (auto& r : k.authority().residents()) if (r.label == label) return r.id;
  return EntityID();
}

// A relative plugin or program path ("./mind_reflect.so") is looked for where
// the caller is first, then beside the running binary, which is where the
// build puts the plugins it makes: a script that names them works from any
// working directory.
static std::string beside_binary(Ctx& c, const std::string& path) {
  if (path.empty() || path[0] == '/' || c.k.host().exists(path)) return path;
#if !defined(AIOS_METAL)
  char exe[4096];
  const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n > 0) {
    std::string dir(exe, (size_t)n);
    dir = dir.substr(0, dir.rfind('/'));
    std::string rel = path.rfind("./", 0) == 0 ? path.substr(2) : path;
    std::string there = dir + "/" + rel;
    if (c.k.host().exists(there)) return there;
  }
#endif
  return path;
}

void cmd_mind(Ctx& c, const Args& a) {
  const std::string sub0 = a.size() > 1 ? a[1] : "status";
  // `mind status` and `mind set` are about the machine, not about one entity,
  // so they are answered before an entity token is demanded.
  if (sub0 == "status" || sub0 == "info") {
    c.view();
    EntityID me = resident_mind_entity(c.k);
    c.o << c.o.style(Header()) << "AIOS resident mind" << c.o.style(Reset()) << "\n";
    if (me.is_null()) { c.o << "  no resident entity labelled 'mind' on this kernel\n"; return; }
    const MindBinding* b = c.k.mind(me);
    c.o << "  Entity        : " << me.short_str() << "\n";
    c.o << "  Bound engine  : ";
    if (b && b->mind) c.o << c.o.style(Green()) << b->mind->name() << c.o.style(Reset())
                          << "  (" << b->cycles << " cycle(s) run)\n";
    else c.o << c.o.style(Yellow()) << "NOTHING BOUND" << c.o.style(Reset()) << "\n";
    // What the settings file asks for, which is not the same claim as what is
    // bound: printing only one of the two is how a machine came to report a
    // model it was not running.
    const char* p_env = std::getenv("AIOS_MODEL_PROVIDER");
    const char* m_env = std::getenv("AIOS_MODEL");
    c.o << "  Configured    : ";
    if (p_env || m_env) c.o << (p_env ? p_env : "(inherited)") << "/" << (m_env ? m_env : "(inherited)")
                            << "  from the environment\n";
    else c.o << "see AOS_PROVIDER / AOS_MODEL in the machine's aos.conf\n";
    c.o << "  Change it     : mind set <provider> [model]   e.g. mind set claude-cli sonnet\n";
    return;
  }
#if !defined(AIOS_METAL)
  if (sub0 == "set" || sub0 == "provider" || sub0 == "model") {
    if (a.size() < 3) fail(Status::Invalid, "mind set <provider> [model] | mind provider <p> | mind model <m>");
    need(c.me.operate());
    std::string provider, model;
    if (sub0 == "provider") provider = a[2];
    else if (sub0 == "model") model = a[2];
    else { provider = a[2]; if (a.size() > 3) model = a[3]; }
    if (!provider.empty()) { set_machine_conf(c.k, "AOS_PROVIDER", provider); setenv("AIOS_MODEL_PROVIDER", provider.c_str(), 1); }
    if (!model.empty())    { set_machine_conf(c.k, "AOS_MODEL", model);       setenv("AIOS_MODEL", model.c_str(), 1); }
    auto sw = hot_swap_mind(c.s, "mind");
    if (!sw.ok()) fail(Status::IoError, "wrote the setting, but the hot swap failed: " + sw.error().str());
    EntityID me = resident_mind_entity(c.k);
    const MindBinding* b = me.is_null() ? nullptr : c.k.mind(me);
    c.o << "mind is now " << c.o.style(Green())
        << ((b && b->mind) ? b->mind->name() : std::string("(nothing bound)")) << c.o.style(Reset())
        << " and aos.conf will bring it back this way on the next boot\n";
    return;
  }
#endif
  if (a.size() < 3) fail(Status::Invalid, "mind status | mind set <provider> [model] | mind bind <e> echo|plugin <path>|extern <cmd...>|wire|qwen [<url>] [<model>]|jepa [out=<ch>] | mind swap <e> ... | mind cycle <e> [n] | mind unbind <e>");
  const std::string& sub = a[1];
  EntityID e = c.r.as<EntityID>(a[2]);
  if (sub == "bind" || sub == "swap") {
    if (a.size() < 4) fail(Status::Invalid, "mind " + sub + " <e> echo|plugin <path>|extern <cmd...>|wire|qwen [<url>] [<model>]|jepa");
    ChannelID out; if (has_kv(a, "out")) out = c.r.as<ChannelID>(kv(a, "out"));
    std::unique_ptr<Intelligence> mind;
    if (a[3] == "echo") mind = make_echo_intelligence();
    else if (a[3] == "wire") mind = need(make_wire_intelligence());
    else if (a[3] == "plugin") { if (a.size() < 5) fail(Status::Invalid, "plugin needs a path"); mind = need(load_intelligence_plugin(beside_binary(c, c.r.expand(a[4])))); }
    else if (a[3] == "extern") {
      std::vector<std::string> argv;
      for (size_t i = 4; i < a.size(); ++i) if (a[i].rfind("out=", 0) != 0) argv.push_back(c.r.expand(a[i]));
      if (!argv.empty() && argv[0].find('/') != std::string::npos) argv[0] = beside_binary(c, argv[0]);
      mind = need(spawn_external_intelligence(argv));
    } else if (a[3] == "local") {
      // The in-tree model, in this process: loaded now so a missing checkpoint
      // is reported here rather than at the first cycle.
      auto* lm = need(userland::resident_llm(c.k.host(), c.k.config().root));
      c.o << "in-tree model " << userland::resident_llm_source() << " (" << lm->config().layers << " layers)\n";
      mind = intelligence::make_qwen_intelligence("local", a.size() > 4 ? c.r.expand(a[4]) : "spark");
    } else if (a[3] == "qwen" || a[3] == "spark" || a[3] == "llm" || a[3] == "net" || a[3] == "network" || a[3] == "ollama" || a[3] == "llava" || a[3] == "gemma" || a[3] == "llama") {
      std::string model = a.size() > 5 ? c.r.expand(a[5]) : (a[3] == "llava" ? "llava:7b" : (a[3] == "gemma" ? "gemma3:4b" : (a[3] == "llama" ? "llama3.2:3b" : "spark")));
      // No URL given: ask the endpoints which of them serves this model,
      // instead of guessing a host from the letters in its name.
      std::string url = a.size() > 4 ? c.r.expand(a[4]) : "";
      if (url.empty()) {
        std::string note;
        url = resolve_model_endpoint(model, &note);
        if (url.empty()) fail(Status::NotFound, note + " — give the URL: mind bind <e> qwen <url> <model>");
        c.o << note << "\n";
      }
      auto net = intelligence::make_qwen_intelligence(url, model);
      // The credential comes off the machine, not off this command line: an
      // argument here lands in the console transcript, the desktop's shell
      // pane and session.log. This is what lets a machine with nothing but a
      // NIC reach a real model API — which on bare metal is the only way it
      // reaches one at all without a serial cable to another host.
      const std::string key = intelligence::network_mind_credential(c.k.host(), c.k.config().root);
      if (!key.empty()) intelligence::set_qwen_credential(*net, key);
      c.o << (key.empty()
                  ? "no credential at " + c.k.config().root + "/mind.key - this mind can only reach an endpoint that needs none\n"
                  : "credential read from " + c.k.config().root + "/mind.key (" + std::to_string(key.size()) + " chars, never printed)\n");
      mind = std::move(net);
    } else if (a[3] == "jepa" || a[3] == "world-model" || a[3] == "jepa-cuda") {
      mind = intelligence::make_jepa_world_model_intelligence();
    } else fail(Status::Invalid, "mind kinds: echo | plugin <path> | extern <cmd...> | wire | local [<name>] | qwen (net, llm, ollama, spark, llava, gemma, llama) [<url>] [<model>] | jepa");
    std::string name = mind->name();
    if (sub == "swap") {
      need(c.me.swap_mind(e, std::move(mind), out));
      c.o << "swapped " << name << " on " << e.short_str() << (out.is_null() ? "" : " publishing to " + out.short_str()) << "\n";
    } else {
      need(c.me.bind_mind(e, std::move(mind), out));
      c.o << "bound " << name << " to " << e.short_str() << (out.is_null() ? "" : " publishing to " + out.short_str()) << "\n";
    }
  } else if (sub == "cycle") {
    uint64_t n = a.size() > 3 ? to_u64(a[3]) : 1;
    for (uint64_t i = 0; i < n; ++i) {
      auto rep = need(c.me.cycle(e));
      c.o << "cycle " << (i + 1) << " [" << rep.intelligence << "] context " << rep.context.short_str() << " → perceived " << rep.perceived.short_str();
      if (rep.computed) c.o << " → graph " << rep.graph.short_str() << " (" << rep.execution.operations_run << " ops, " << fmt_ns(rep.execution.elapsed_ns) << ")";
      else c.o << " → no computation";
      c.o << " → result " << rep.result.short_str() << (rep.published ? " published #" + std::to_string(rep.published_sequence) : "") << "  " << fmt_ns(rep.elapsed_ns) << "\n";
      if (rep.computed) for (size_t j = 0; j < rep.execution.samples.size(); ++j) { auto& s = rep.execution.samples[j]; auto& p = rep.execution.placements[j]; c.o << "        op " << s.op.short_str() << " → " << p.device_name << " lanes=" << s.lanes_used << " " << fmt_ns(s.finished_ns - s.started_ns) << "\n"; }
      auto st = c.k.states().copy(rep.result);
      if (st.ok()) { c.o << "        result: " << c.bytes_line(st->payload) << "\n"; c.s.vars["read"] = std::string(st->payload.begin(), st->payload.end()); }
      c.produced(rep.result.str());
    }
  } else if (sub == "unbind") {
    need(c.me.unbind_mind(e));
    c.o << "unbound\n";
  } else fail(Status::Invalid, "unknown: mind " + sub);
}


// ---------------------------------------------------------------------------
// The machine's own language model. `lm` is the whole surface: what it is, what
// it costs, whether its hand-written derivatives are right, and what it says.
// ---------------------------------------------------------------------------

// The intelligence core has no filesystem, so the machine registers the two
// hooks its language model persists through. Every path that can reach the
// model calls this first — it was inside `lm` alone, which meant a plain
// question trained a fresh model for 60 steps and then answered from it, at
// 2.7 nats/token instead of the 0.12 the trained weights give.
namespace {
Ctx* g_lm_store_ctx = nullptr;
void bind_language_model_store(Ctx& c) {
  g_lm_store_ctx = &c;
  intelligence::set_lm_store(
      [](const char* name, std::string& out) -> bool {
        if (!g_lm_store_ctx) return false;
        auto r = g_lm_store_ctx->k.host().read_file(g_lm_store_ctx->k.config().root + "/" + name + ".bin");   // host-io: the lm checkpoint under the kernel root
        if (!r.ok()) return false;
        out.assign((const char*)r->data(), r->size());
        return true;
      },
      [](const char* name, const std::string& blob) -> bool {
        if (!g_lm_store_ctx) return false;
        return g_lm_store_ctx->k.host().write_file(g_lm_store_ctx->k.config().root + "/" + name + ".bin",   // host-io: the lm checkpoint under the kernel root
                                                   ByteSpan((const uint8_t*)blob.data(), blob.size())).ok();
      });
}
}  // namespace

// The likelihood under which the console acts on the model's mapping of a
// request to a command (see the unknown-command path in run_line). `lm ask`
// reports against the same bar.
constexpr float kLmConfidentNll = 1.0f;

void cmd_lm(Ctx& c, const Args& a) {
  const std::string sub = a.size() > 1 ? a[1] : "status";
  auto* m = intelligence::get_machine_language_model();

  bind_language_model_store(c);

  if (sub == "status") {
    std::string what;
    const bool ready = m->ensure_ready(&what);
    c.o << "The machine's own language model\n";
    c.o << "  state           : " << (ready ? "ready" : "NOT ready") << "\n";
    c.o << "  " << what << "\n";
    c.o << "  vocabulary      : " << m->tokenizer().vocab_size() << " tokens ("
        << m->tokenizer().merge_count() << " learned merges over 256 bytes)\n";
    const auto& cfg = m->model().config();
    c.o << "  shape           : d_model " << cfg.d_model << ", " << cfg.n_layer << " layer(s), "
        << cfg.n_head << " head(s), d_ff " << cfg.d_ff << ", context " << cfg.block << "\n";
    c.o << "  parameters      : " << m->model().parameter_count() << "\n";
    c.o << "  trained for     : " << m->model().train_steps() << " step(s), last loss "
        << m->model().last_loss() << "\n";
    c.o << "  corpus          : " << m->corpus_lines() << " line(s) this machine wrote about itself\n";
    c.o << "  weights persist : " << (intelligence::lm_store_is_live() ? "yes, under the kernel root" : "no") << "\n";
    // Said plainly, because a small model that is described as more than it is
    // becomes the same lie as a status surface that prints a constant.
    c.o << "  what it does    : maps a request onto one of the console commands it was\n";
    c.o << "                    trained on, by scoring every one of them under the model,\n";
    c.o << "                    and says a sentence about the current state vector.\n";
    c.o << "                    It is not a general assistant and cannot answer as one.\n";
    return;
  }

  if (sub == "train") {
    // lm train [steps] [lr] [steps=N] [lr=X] [save_every=N] [corpus=<file>] [engine=scalar] [device=...]
    // Positionals are the ones that are not key=value, so `lm train 5
    // engine=scalar` is 5 steps at the default rate, and `lm train
    // engine=scalar` is the default 200 steps.
    std::vector<std::string> pos;
    for (size_t i = 2; i < a.size(); ++i) if (a[i].find('=') == std::string::npos) pos.push_back(a[i]);
    auto num = [&](const std::string& text, const char* what) {
      char* end = nullptr;
      double v = std::strtod(text.c_str(), &end);
      if (!end || *end != '\0' || text.empty()) fail(Status::Invalid, std::string("lm train: ") + what + " '" + text + "' is not a number");
      return v;
    };
    int steps = 200;
    float lr = 3e-3f;
    if (pos.size() > 0) steps = (int)num(pos[0], "steps");
    if (pos.size() > 1) lr = (float)num(pos[1], "lr");
    if (pos.size() > 2) fail(Status::Invalid, "lm train [steps] [lr] [key=value...]: '" + pos[2] + "' is extra");
    if (has_kv(a, "steps")) steps = (int)num(kv(a, "steps"), "steps");
    if (has_kv(a, "lr")) lr = (float)num(kv(a, "lr"), "lr");
    if (steps < 1) fail(Status::Invalid, "lm train: steps must be at least 1");
    if (!(lr > 0.0f) || lr > 1.0f) fail(Status::Invalid, "lm train: lr must be in (0, 1]");
    int save_every = has_kv(a, "save_every") ? (int)num(kv(a, "save_every"), "save_every") : 50;
    std::string what;
    if (!m->ensure_ready(&what)) fail(Status::Unsupported, "the model has no corpus to train on");
    c.o << what << "\n";
    // Training happens on the model the machine is holding. The loss printed is
    // the mean over the batch.
    auto* mm = const_cast<intelligence::TinyTransformerLM*>(&m->model());
    const auto& tok = m->tokenizer();
    std::vector<std::vector<int>> encoded;
    // The text: the corpus the machine built for itself, or corpus=<file> —
    // your own text, one training sequence per non-empty line, encoded with
    // the same tokenizer (any bytes encode; unseen words just take more tokens).
    std::vector<std::string> lines;
    std::string corpus_name = "the built-in corpus";
    if (has_kv(a, "corpus")) {
      const std::string path = c.r.expand(kv(a, "corpus"));
      auto file = c.k.host().read_file(path);   // host-io: the corpus file the operator named (lm train corpus=)
      if (!file.ok()) fail(Status::NotFound, "corpus " + path + ": " + file.error().message);
      std::string text(file->begin(), file->end());
      size_t at0 = 0;
      while (at0 < text.size()) {
        size_t nl = text.find('\n', at0);
        std::string line = text.substr(at0, nl == std::string::npos ? std::string::npos : nl - at0);
        at0 = nl == std::string::npos ? text.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line + "\n");
      }
      corpus_name = path + " (" + std::to_string(lines.size()) + " line(s))";
    } else {
      lines = intelligence::machine_corpus();
    }
    const int block = mm->config().block;
    for (const auto& line : lines) {
      auto ids = tok.encode(line);
      // A line longer than the context is split into context-sized windows.
      for (size_t from = 0; from + 1 < ids.size(); from += (size_t)block) {
        std::vector<int> piece(ids.begin() + (long)from, ids.begin() + (long)std::min(ids.size(), from + (size_t)block));
        if (piece.size() >= 2) encoded.push_back(std::move(piece));
      }
    }
    if (encoded.empty()) fail(Status::Unsupported, "the corpus encoded to nothing");
    c.o << "training on " << corpus_name << ": " << encoded.size() << " sequence(s), " << steps << " step(s) at lr "
        << lr << (save_every > 0 ? ", saved every " + std::to_string(save_every) + " step(s)" : std::string(", saved at the end")) << "\n";
    const float before = mm->last_loss();
    const uint64_t steps_before = mm->train_steps();
    size_t at = 0;
    float loss = before;
    // Where the operations may run: `device=cuda`, `device=cpu`, `device=remote:`
    // pins every operation to devices whose name starts that way; unset lets
    // the scheduler place each one. `engine=scalar` is the hand-written CPU
    // pass, kept as the reference the graph is checked against.
    const std::string selector = kv(a, "device", "");
    const bool scalar = kv(a, "engine", "graph") == "scalar";
    intelligence::TinyTransformerLM::GraphTrainReport total_rep;
    int saves = 0;
    for (int step = 0; step < steps; ++step) {
      std::vector<std::vector<int>> batch;
      for (int b = 0; b < 4; ++b) { batch.push_back(encoded[at % encoded.size()]); ++at; }
      if (scalar) {
        loss = mm->train_step(batch, lr);
      } else {
        intelligence::TinyTransformerLM::GraphTrainReport rep;
        loss = mm->train_step_graph(batch, lr, c.k.scheduler(), c.k.memory(), c.k.ids(), c.k.self(), &rep, selector);
        if (!rep.failure.empty()) {
          c.o << "  step " << (step + 1) << ": the graph did not run — " << rep.failure << "\n";
          if (mm->train_steps() > steps_before) intelligence::persist_machine_language_model();
          fail(Status::IoError, "training stopped after " + std::to_string(mm->train_steps() - steps_before) +
                                    " completed step(s): " + rep.failure);
        }
        total_rep.operations += rep.operations;
        total_rep.elapsed_ns += rep.elapsed_ns;
        for (auto& [dev, n] : rep.per_device) total_rep.per_device[dev] += n;
      }
      if ((step + 1) % 25 == 0 || step + 1 == steps) {
        c.o << "  step " << (step + 1) << "/" << steps << "  loss " << loss << "\n";
      }
      // Periodically, so an interrupted run keeps what it learned.
      if (save_every > 0 && (step + 1) % save_every == 0 && step + 1 < steps) {
        intelligence::persist_machine_language_model();
        ++saves;
      }
    }
    c.o << "loss " << before << " -> " << loss << " over " << steps << " step(s) at lr " << lr << "\n";
    c.o << "total steps " << mm->train_steps() << ", " << mm->parameter_count() << " parameters\n";
    if (!scalar) {
      // What actually ran, and where. This is the evidence that the step was
      // a graph over the fabric and not the scalar loop under another name.
      c.o << "graph: " << total_rep.operations << " operation(s) in " << fmt_double((double)total_rep.elapsed_ns / 1e6, 8, true, 1) << " ms on";
      for (auto& [dev, n] : total_rep.per_device) c.o << " [" << dev << " x" << n << "]";
      c.o << "\n";
    } else {
      c.o << "engine: the scalar CPU pass (no graph, no device)\n";
    }
    intelligence::persist_machine_language_model();
    ++saves;
    c.o << (intelligence::lm_store_is_live()
                ? "weights and optimizer state written under the kernel root (" + std::to_string(saves) + " save(s))\n"
                : std::string("no store registered: these weights are gone at exit\n"));
    return;
  }

  if (sub == "tokenize") {
    std::string text;
    for (size_t i = 2; i < a.size(); ++i) { if (i > 2) text += " "; text += a[i]; }
    if (text.empty()) fail(Status::Invalid, "lm tokenize <text>");
    std::string what;
    m->ensure_ready(&what);
    const auto ids = m->tokenizer().encode(text);
    c.o << ids.size() << " token(s) for " << text.size() << " byte(s):\n";
    for (int id : ids) {
      std::string piece = m->tokenizer().decode({id});
      std::string shown;
      for (char ch : piece) shown += (ch == '\n') ? std::string("\\n") : std::string(1, ch);
      c.o << "  " << id << "  \"" << shown << "\"\n";
    }
    const std::string back = m->tokenizer().decode(ids);
    c.o << (back == text ? "round trip: identical\n" : "round trip: DIFFERS - " + back + "\n");
    return;
  }

  if (sub == "check") {
    // A hand-written backward pass that nobody checked is a plausible backward
    // pass. This compares the analytic gradient against a central finite
    // difference on a sample of parameters; if the maths is wrong, the relative
    // error is not small and this says so.
    std::string what;
    m->ensure_ready(&what);
    auto* mm = const_cast<intelligence::TinyTransformerLM*>(&m->model());
    const auto ids = m->tokenizer().encode("what do you hold >>> info\n");
    if (ids.size() < 3) fail(Status::Unsupported, "the probe sequence encoded to nothing");
    int samples = 12, sabotage = -1;
    for (size_t i = 2; i < a.size(); ++i) if (a[i].find('=') == std::string::npos) samples = std::atoi(a[i].c_str());
    if (has_kv(a, "sabotage")) sabotage = std::atoi(kv(a, "sabotage").c_str());
    const auto report = intelligence::gradient_check(*mm, ids, samples, sabotage);
    if (report.sabotaged >= 0)
      c.o << "SABOTAGED: parameter tensor " << report.sabotaged << "'s analytic gradient is negated on purpose\n";
    c.o << "Gradient check against central differences (h = 1e-3):\n";
    for (const auto& r : report.entries) {
      c.o << "  " << r.what << "  analytic " << r.analytic << "  numeric " << r.numeric
          << "  rel err " << r.rel_err << "\n";
    }
    c.o << "  resolution floor: " << report.floor
        << " (the loss is a float32; a central difference cannot see a gradient under this)\n";
    c.o << "  skipped as unmeasurable: " << report.skipped << "\n";
    c.o << "  worst relative error over the rest: " << report.worst << "\n";
    c.o << "Directional derivative over ALL " << m->model().parameter_count()
        << " parameters at once (h = 1e-2) - this is the test that decides,\n";
    c.o << "because a single parameter's difference in a float32 loss is mostly noise:\n";
    for (const auto& d : report.directions) {
      c.o << "  analytic g.d " << d.analytic << "  numeric dL/da " << d.numeric << " (half step " << d.numeric_half
          << ")  |diff| " << std::fabs(d.numeric - d.analytic) << " vs tolerance " << d.tolerance
          << (d.agrees ? "  agrees" : "  DISAGREES") << "\n";
    }
    c.o << "  tolerance = 2% of the magnitude + the float32 floor + twice the numeric side's own spread\n";
    c.o << "  worst relative error over " << report.directions.size() << " random direction(s): " << report.dir_worst << "\n";
    const bool ok = report.dir_pass;
    c.o << (ok ? "the derivatives agree with the function they are supposed to differentiate\n"
               : "THE DERIVATIVES DO NOT AGREE - the backward pass is wrong\n");
    return;
  }

  if (sub == "ask" || sub == "say") {
    std::string text;
    for (size_t i = 2; i < a.size(); ++i) { if (i > 2) text += " "; text += a[i]; }
    if (text.empty()) fail(Status::Invalid, "lm ask <request>");
    const auto reply = intelligence::translate_request(text);
    // The console acts on the model's choice only under kLmConfidentNll; this
    // says the same thing the console would do.
    const bool confident = reply.used_model && reply.command_nll < kLmConfidentNll;
    c.o << "request     : " << text << "\n";
    if (confident) {
      c.o << "answered by : the model\n";
      c.o << "command     : " << reply.command << "\n";
    } else if (reply.used_model) {
      c.o << "answered by : nobody with confidence — the model's best guess is above the " << kLmConfidentNll
          << " nats/token the console requires, so the console would not act on it\n";
      c.o << "best guess  : " << reply.command << "\n";
      c.o << "keywords say: " << intelligence::keyword_fallback_command(text) << "\n";
    } else {
      c.o << "answered by : the keyword fallback\n";
      c.o << "command     : " << reply.command << "\n";
    }
    if (reply.used_model) c.o << "chosen at   : " << reply.command_nll << " nats/token\n";
    c.o << "generated   : \"" << reply.generated << "\"\n";
    c.o << "state       : " << reply.explanation << "\n";
    return;
  }

  fail(Status::Invalid, "lm status | train [steps] [lr] [corpus=<file>] [save_every=N] [engine=scalar] | tokenize <text> | check [n] [sabotage=<slot>] | ask <request>");
}



// ---------------------------------------------------------------------------
// The levers a HOSTED machine actually owns.
// ---------------------------------------------------------------------------
// Only bare metal registered an actuation sink, because a hosted process does
// not own the scheduler or the NIC ring — which is true, and was taken to mean
// a hosted machine has no levers at all. It has two: what goes on its own
// display, and the quota its scheduler charges an entity before making it wait.
// This applies those and DECLINES the rest by name, so `jepa status` can say
// which of the four landed instead of implying all of them did.
namespace {
Kernel* g_act_kernel = nullptr;

// When a person last said what they wanted on the screen. The policy may
// explore its other levers freely, but "show me a pyramid" is an INSTRUCTION,
// not an optimisation: there is no measurable sense in which one scene is a
// better scene, so there is nothing for a reward to prefer and nothing that
// justifies the policy overriding what was asked for. Exploring a lever whose
// effect a person is looking at, against their instruction, is not exploration.
uint64_t g_display_pinned_until_ms = 0;
intelligence::SubstrateActuation g_act_last;
bool g_act_ever = false;
// The policy still ASKS for a scene every step — that is the learning loop
// working, and it is counted here so it stays visible — but the ask is never
// applied. Who owns the screen is the person.
uint32_t g_act_display_declined = 0;
uint32_t g_act_quota_writes = 0;

std::string face_file_path() { return run_dir_path() + "/face"; }

std::string scene_file_path() { return run_dir_path() + "/scene"; }

const char* scene_name(uint32_t n) {
  switch (n) {
    case 1: return "cyber";
    case 2: return "orb";
    case 3: return "pyramid";
    default: return "beach";
  }
}

bool hosted_actuate(const intelligence::SubstrateActuation& act) {
  if (!g_act_kernel) return false;
  bool any = false;

  // 1. The display. DECLINED, always — the operator owns what is on the glass.
  //    a_t[2] is still decoded and reported so the lever stays visible, but
  //    nothing here writes the scene file: the shown scene changes only on an
  //    explicit `display` request or from the persisted desk state. A policy
  //    head that repaints the screen because a reward moved is the machine
  //    taking the display off the person sitting in front of it, and there is
  //    no pin or timeout that makes that acceptable — so it is not gated, it
  //    is gone.
  //    The ask is COUNTED, not silently dropped: the policy head goes on
  //    proposing a_t[2] and goes on learning from everything else it moves, and
  //    `jepa policy` shows how often it wanted the screen and did not get it.
  if (!g_act_ever || act.display_scene != g_act_last.display_scene) ++g_act_display_declined;

  // 2. The quota, on every entity that is not the kernel itself. The scheduler
  //    charges measured device time against it and makes the entity wait when
  //    the window is spent, so a percentage here is a real bound on load.
  if (!g_act_ever || act.compute_quota_pct != g_act_last.compute_quota_pct) {
    const uint64_t period_ns = 1000000000ull;
    const uint64_t budget_ns = period_ns * (uint64_t)act.compute_quota_pct / 100ull;
    const uint64_t slice_ns = budget_ns / 8ull ? budget_ns / 8ull : 1000000ull;
    for (const auto& e : g_act_kernel->entities().list()) {
      if (e.id == g_act_kernel->self()) continue;
      if (g_act_kernel->quotas().assign_quota(e.id, slice_ns, budget_ns, period_ns).ok()) {
        ++g_act_quota_writes;
        any = true;
      }
    }
  }

  // 3 and 4. The scheduler slice and the NIC refill window belong to whoever
  //    owns the timer and the ring, and hosted that is Linux. Not applied, and
  //    not pretended: `jepa status` names them as declined here.
  g_act_last = act;
  g_act_ever = true;
  return any;
}

void bind_substrate_actuation(Ctx& c) {
  if (g_act_kernel) return;
  // NEVER take the sink off a substrate that registered its own.
  //
  // This replaced it unconditionally, and on bare metal that meant the first
  // `jepa` command silently disconnected `metal_actuate` — the sink that sets
  // the scheduler slice — and put a sink in its place that writes a scene FILE
  // on a machine with no filesystem and assigns quotas. So a_t[0] stopped
  // reaching the scheduler entirely: `jepa sweep` read the slice HELD as 1 at
  // every setting while the policy asked for 1 through 16, the switch rate did
  // not move because nothing had been changed, and the metal learning run
  // reported a reward with no gradient. The reward was right the whole time.
  // The lever had been unplugged by this function.
  //
  // The rule is the honest one: the CLI fills in only where the substrate left
  // the hook empty.
  if (intelligence::actuation_is_live()) return;
  g_act_kernel = &c.k;
  intelligence::set_actuation_sink(&hosted_actuate);
}

struct ActuationReport { uint32_t display_declined, quota_writes; intelligence::SubstrateActuation last; bool ever; };
ActuationReport actuation_report() { return {g_act_display_declined, g_act_quota_writes, g_act_last, g_act_ever}; }

// Which of the four levers the actuation sink bound in THIS process applies.
// 1 = applied, 0 = declined, -1 = a sink this file did not install, so unknown.
struct LeverOwnership { const char* sink; int slice, nic, display, quota; };
LeverOwnership lever_ownership() {
  if (g_act_kernel) return {"hosted (this CLI's sink: quota only)", 0, 0, 0, 1};
  if (intelligence::actuation_is_live()) {
#if defined(AIOS_METAL)
    // metal_actuate (metal/main.cpp): slice, display scene and NIC refill; no quota.
    return {"bare metal (metal_actuate)", 1, 1, 1, 0};
#else
    return {"a substrate sink this CLI did not install", -1, -1, -1, -1};
#endif
  }
  return {"none bound", 0, 0, 0, 0};
}
const char* lever_word(int v) { return v > 0 ? "APPLIED here" : v == 0 ? "DECLINED here" : "unknown here"; }
}  // namespace

// ---------------------------------------------------------------------------
// The predictor's matrix products, on this machine's GPU.
// ---------------------------------------------------------------------------
// The intelligence core declares a hook and never reaches for a device; this is
// the substrate filling it in. Each product allocates three objects in the
// memory fabric, writes the operands, hands one matmul Operation to a device
// that says it supports it, and reads the result back. Declining is a legal
// answer and the core then does the product itself — a device that returned an
// untouched buffer would be a wrong answer wearing an accelerator's badge.
namespace {
Kernel* g_mm_kernel = nullptr;
Device* g_mm_device = nullptr;

// Below this, the device loses and declining is the right answer. It is not a
// guess: `jepa bench` on this machine measures 0.64x at batch 32 (37M
// multiply-accumulates a product), 0.89x at 64, 1.33x at 128 and 1.84x at 256.
// The operands have to be allocated, written across the bus and read back every
// call, and under about 50M MACs that costs more than the arithmetic saves.
// Above it the advantage grows with the batch, which is what a fixed transfer
// cost amortised over more work looks like.
constexpr uint64_t kOffloadWorthIt = 48ull * 1000ull * 1000ull;
// `jepa bench` is measuring the device deliberately, including at sizes where
// it loses — that measurement is the whole point — so it lifts the floor.
bool g_mm_take_everything = false;

bool fabric_matmul(const float* A, const float* B, float* C, int M, int K, int N) {
  if (!g_mm_kernel || !g_mm_device) return false;
  if (!g_mm_take_everything && (uint64_t)M * (uint64_t)K * (uint64_t)N < kOffloadWorthIt) return false;
  auto& mem = g_mm_kernel->memory();
  const EntityID owner = g_mm_kernel->self();
  const uint64_t abytes = (uint64_t)M * K * sizeof(float);
  const uint64_t bbytes = (uint64_t)K * N * sizeof(float);
  const uint64_t cbytes = (uint64_t)M * N * sizeof(float);

  auto ma = mem.allocate(owner, abytes);
  if (!ma.ok()) return false;
  auto mb = mem.allocate(owner, bbytes);
  if (!mb.ok()) { (void)mem.release(*ma); return false; }
  auto mc = mem.allocate(owner, cbytes);
  if (!mc.ok()) { (void)mem.release(*ma); (void)mem.release(*mb); return false; }
  struct Release {
    MemoryFabric& m; MemoryID a, b, c;
    ~Release() { (void)m.release(a); (void)m.release(b); (void)m.release(c); }
  } release{mem, *ma, *mb, *mc};

  if (!mem.write(*ma, 0, ByteSpan((const uint8_t*)A, abytes)).ok()) return false;
  if (!mem.write(*mb, 0, ByteSpan((const uint8_t*)B, bbytes)).ok()) return false;

  Operation op;
  op.id = g_mm_kernel->ids().next_as<OperationID>();
  op.code = OpCode::MatMul;
  op.inputs.push_back(Tensor{*ma, DType::F32, {(uint64_t)M, (uint64_t)K}, 0});
  op.inputs.push_back(Tensor{*mb, DType::F32, {(uint64_t)K, (uint64_t)N}, 0});
  op.outputs.push_back(Tensor{*mc, DType::F32, {(uint64_t)M, (uint64_t)N}, 0});
  if (!g_mm_device->supports(op)) return false;
  if (!g_mm_device->execute(op, mem, 1).ok()) return false;

  auto out = mem.read(*mc, 0, cbytes);
  if (!out.ok() || out->size() != cbytes) return false;
  std::memcpy(C, out->data(), cbytes);
  return true;
}

// Pick the device once: a GPU that says it can do a matmul, preferring one
// whose name says it is actually dispatching rather than simulating.
void bind_predictor_offload(Ctx& c, bool on) {
  if (!on) {
    g_mm_kernel = nullptr;
    g_mm_device = nullptr;
    intelligence::set_matmul_offload(nullptr, nullptr);
    return;
  }
  g_mm_kernel = &c.k;
  g_mm_device = nullptr;
  Operation probe;
  probe.code = OpCode::MatMul;
  probe.inputs.push_back(Tensor{MemoryID{}, DType::F32, {4, 4}, 0});
  probe.inputs.push_back(Tensor{MemoryID{}, DType::F32, {4, 4}, 0});
  probe.outputs.push_back(Tensor{MemoryID{}, DType::F32, {4, 4}, 0});
  for (auto* d : c.k.devices().of_type(DeviceType::GPU)) {
    if (!d->supports(probe)) continue;
    if (d->name().rfind("vulkan:", 0) == 0 || d->name().find("Vulkan") != std::string::npos) { g_mm_device = d; break; }
    if (!g_mm_device) g_mm_device = d;
  }
  if (!g_mm_device) {
    intelligence::set_matmul_offload(nullptr, nullptr);
    return;
  }
  static std::string what;
  what = g_mm_device->name() + " through the compute fabric";
  intelligence::set_matmul_offload(&fabric_matmul, what.c_str());
}
}  // namespace


// ---------------------------------------------------------------------------
// What this machine puts on its own display.
// ---------------------------------------------------------------------------
// The scenes were reachable only by pressing a key at the glass. They are a
// lever now — one a person can name, one the language model can be asked for in
// a sentence, and one the policy head can pull through a_t[2] — because a lever
// whose effect you can SEE is the only kind nobody has to take on trust.

// ---------------------------------------------------------------------------
// Checking the network and correcting it; checking the load and correcting it.
// ---------------------------------------------------------------------------
// Both are the same shape, and it is the shape this repo insists on: measure,
// act, measure again, and report the difference rather than the intention. A
// "repair" that prints "network optimised" without a before and an after is a
// status surface printing a constant.
void cmd_net_correct(Ctx& c, const Args& a) {
  bind_substrate_actuation(c);
  const std::string sub = a.size() > 1 ? a[1] : "check";

  auto sample = [&](int ms) {
    // Two reads with real time between them: these counters are rates, and a
    // rate needs an interval. The provider itself differences against its own
    // previous read, so the first is primed and thrown away.
    (void)intelligence::read_substrate_telemetry();
    hal::sleep_ms((uint32_t)ms);
    return intelligence::read_substrate_telemetry();
  };

  if (sub == "check") {
    const auto t = sample(a.size() > 2 ? std::max(100, std::atoi(a[2].c_str())) : 700);
    c.o << "Network, as this machine's own counters report it\n";
    c.o << "  measured   : " << (intelligence::telemetry_is_live() ? "yes, from the substrate's provider"
                                                                   : "NO - no provider registered, these are synthetic") << "\n";
    c.o << "  packet loss: " << (t.packet_loss * 100.0f) << "% of frames dropped or errored\n";
    c.o << "  rx flow    : " << (t.nic_ring_depth * 100.0f) << "% of 100 MB/s\n";
    auto nets = c.k.host().network();
    if (nets.ok()) {
      for (const auto& n : *nets) {
        if (n.loopback) continue;
        c.o << "  " << n.name << "  " << (n.up ? "up" : "down") << "  " << n.address << "\n";
      }
    }
    const auto act = intelligence::decode_action(std::vector<float>(4, 0.0f));
    c.o << "  refill window the policy would ask for at a_t[1]=0: " << act.nic_refill_window << " buffer(s)\n";
    if (t.packet_loss <= 0.0f) {
      c.o << "nothing is being lost, so there is nothing to correct\n";
    } else {
      c.o << "there is loss to correct: `net repair` widens the refill window and measures again\n";
    }
    return;
  }

  if (sub == "repair" || sub == "correct") {
    const auto before = sample(700);
    c.o << "packet loss before: " << (before.packet_loss * 100.0f) << "%\n";
    // The correction is the lever the policy has: keep more receive buffers
    // posted, so a burst has somewhere to land. Asked for through the action
    // vector, not by reaching past it, because that is the interface the policy
    // head uses and it should be the same one a person uses.
    std::vector<float> a_t(4, 0.0f);
    a_t[1] = 1.0f;                       // the window wide open: 32 buffers
    a_t[3] = 1.0f;                       // and no quota squeeze while we look
    intelligence::SubstrateActuation applied;
    const bool landed = intelligence::apply_action(a_t, &applied);
    c.o << "asked the substrate for a refill window of " << applied.nic_refill_window
        << " buffer(s)" << (landed ? "" : " - NO SINK TOOK IT") << "\n";
    if (!landed) {
      c.o << "hosted, the receive ring belongs to Linux and this machine does not own it;\n"
          << "the window is a real lever on bare metal, where `host jepa nic` counts it.\n";
    }
    const auto after = sample(700);
    c.o << "packet loss after : " << (after.packet_loss * 100.0f) << "%\n";
    const float d = before.packet_loss - after.packet_loss;
    if (before.packet_loss <= 0.0f && after.packet_loss <= 0.0f) {
      c.o << "nothing was being lost either side of that, so nothing was corrected -\n"
          << "which is the honest reading, not a success\n";
    } else if (d > 0.0f) {
      c.o << "loss fell by " << (d * 100.0f) << " percentage point(s)\n";
    } else if (d < 0.0f) {
      c.o << "loss ROSE by " << (-d * 100.0f) << " percentage point(s) - the wider window did not help here\n";
    } else {
      c.o << "loss did not move\n";
    }
    return;
  }

  fail(Status::Invalid, "net check [ms] | net repair");
}

void cmd_load_correct(Ctx& c, const Args& a) {
  bind_substrate_actuation(c);
  (void)a;
  auto read = [&]() {
    (void)intelligence::read_substrate_telemetry();
    hal::sleep_ms(500);
    return intelligence::read_substrate_telemetry();
  };
  const auto before = read();
  c.o << "Load, before\n";
  c.o << "  runnable work per processor : " << (before.load_imbalance * 4.0f) << "\n";
  c.o << "  cpu load                   : " << (before.cpu_load * 100.0f) << "%\n";
  c.o << "  context switches           : " << (before.ctx_switch_rate * 100.0f) << "% of 20k/s\n";

  // The correction: a quota proportional to how overloaded the machine is. Fully
  // loaded, entities are bounded to a third of a period; idle, they are left
  // alone. Asked for through a_t[3], the same lever the policy head pulls.
  const float over = before.load_imbalance;
  std::vector<float> a_t(4, 0.0f);
  a_t[3] = over > 0.5f ? -0.5f : (over > 0.25f ? 0.0f : 1.0f);
  intelligence::SubstrateActuation applied;
  const bool landed = intelligence::apply_action(a_t, &applied);
  c.o << "Correction\n";
  c.o << "  quota asked for : " << applied.compute_quota_pct << "% of measured device time per second\n";
  c.o << "  scheduler slice : " << applied.scheduler_slice_ticks
      << " tick(s) (bare metal owns this one; hosted it is declined)\n";
  c.o << "  landed          : " << (landed ? "yes" : "NO - no sink took it") << "\n";
  if (landed) {
    const auto quotas = c.k.quotas().list();
    c.o << "  entities now bounded: " << quotas.size() << "\n";
    for (const auto& q : quotas) {
      c.o << "    " << q.entity.short_str() << "  budget " << (q.quota.budget_ns / 1000000ull)
          << " ms/period, slice " << (q.quota.timeslice_ns / 1000000ull) << " ms, preemptions "
          << q.quota.preemptions << "\n";
    }
  }
  const auto after = read();
  c.o << "Load, after\n";
  c.o << "  runnable work per processor : " << (after.load_imbalance * 4.0f) << "\n";
  c.o << "  cpu load                   : " << (after.cpu_load * 100.0f) << "%\n";
  const float d = before.load_imbalance - after.load_imbalance;
  c.o << (d > 0.01f ? "the load fell\n"
        : d < -0.01f ? "the load ROSE over that interval - the quota did not cause it to fall\n"
        : "the load did not move over that interval\n");
  c.o << "Said plainly: the quota bounds what an entity of THIS kernel may spend.\n"
      << "It does not bound the Linux processes beside it, and the load average\n"
      << "above counts those too - which is why the number can be unmoved.\n";
}

void cmd_display(Ctx& c, const Args& a) {
  bind_substrate_actuation(c);
  const std::string sub = a.size() > 1 ? a[1] : "status";

  struct Named { const char* name; uint32_t scene; const char* what; };
  static const Named kScenes[] = {
    {"ocean",   0, "a tide under a moon, and the moon's column broken up on the water"},
    {"beach",   0, "a tide under a moon, and the moon's column broken up on the water"},
    {"tide",    0, "a tide under a moon, and the moon's column broken up on the water"},
    {"moon",    0, "a tide under a moon, and the moon's column broken up on the water"},
    {"cyber",   1, "a lattice waking in a wavefront"},
    {"lattice", 1, "a lattice waking in a wavefront"},
    {"matrix",  1, "a lattice waking in a wavefront"},
    {"orb",     2, "a plasma core in its own halo"},
    {"core",    2, "a plasma core in its own halo"},
    {"plasma",  2, "a plasma core in its own halo"},
    {"pyramid", 3, "a square pyramid on a plain, raymarched from its distance field, with the sun to one side"},
    {"desert",  3, "a square pyramid on a plain, raymarched from its distance field, with the sun to one side"},
  };

  // What the composed scene actually says. A scene that cannot be read back is
  // a scene nobody can correct.
  if (sub == "describe" || sub == "description") {
    // What is actually SELECTED, not merely what is lying in the sidecar. This
    // read the .composed file alone, so it printed a composition the machine
    // was not showing — a leftover from an earlier request — and errored out
    // whenever the shown scene was a compiled-in one. The operator's one way of
    // asking "is the glass on what I asked for?" was answering about something
    // else entirely. Who owns the choice is the person, so this reports THEIRS:
    // the request in the scene file, which is what the compositor reads.
    std::string req = "beach";
    {
      auto sf = c.k.host().read_file(scene_file_path());   // host-io: the machine's own run dir (display state the desktop reads)
      if (sf.ok() && sf->size() > 0) {
        const std::string whole((const char*)sf->data(), sf->size());
        const size_t b = whole.find_first_not_of(" \t\r\n");
        const size_t e = (b == std::string::npos) ? b : whole.find_first_of(" \t\r\n", b);
        if (b != std::string::npos)
          req = whole.substr(b, (e == std::string::npos) ? std::string::npos : e - b);
      }
    }
    auto df = c.k.host().read_file(scene_file_path() + ".composed");   // host-io: the machine's own run dir (display state the desktop reads)
    if (req != "composed" && req != "4") {
      // A compiled-in scene is an ANSWER here, not a not-found.
      const char* what = nullptr;
      for (const auto& n : kScenes) if (req == n.name) { what = n.what; break; }
      c.o << "Showing: " << (what ? what : req.c_str()) << "\n";
      c.o << "  scene \"" << req << "\", asked for explicitly; not a composed one\n";
      if (df.ok() && df->size() > 0)
        c.o << "  (a composition from an earlier request is still on disk at "
            << scene_file_path() << ".composed - it is NOT what is shown)\n";
      return;
    }
    if (!df.ok() || df->size() == 0) fail(Status::NotFound, "this machine has not composed a scene yet");
    std::string text((const char*)df->data(), df->size());
    auto parsed = userland::SceneDescription::from_text(text);
    if (!parsed.ok()) fail(parsed.error().status, parsed.error().message);
    c.o << "Composed: " << parsed->summary << "\n";
    c.o << "  ground " << userland::ground_kind_name(parsed->ground)
        << ", " << parsed->solids.size() << " solid(s), fog " << parsed->fog_density << "\n";
    for (const auto& sol : parsed->solids) {
      c.o << "  " << userland::solid_kind_name(sol.kind)
          << " at (" << sol.x << ", " << sol.y << ", " << sol.z << ")"
          << " size (" << sol.sx << ", " << sol.sy << ", " << sol.sz << ")"
          << " gloss " << sol.gloss << "\n";
    }
    c.o << "  the text of it is " << scene_file_path() << ".composed\n";
    return;
  }

  if (sub == "auto" || sub == "release") {
    g_display_pinned_until_ms = 0;
    // Truthful now: releasing the pin gives the screen back to nobody but the
    // next person to ask. a_t[2] is declined whether or not a pin is running,
    // so this must not go on promising that the policy head takes over.
    c.o << "the display pin is released; the scene still changes only on an explicit\n"
        << "request or from the saved desk state - the policy head does not get it back\n";
    return;
  }

  if (sub == "desk" || sub == "panels") {
    static const char kDesk[] = "desk";
    ensure_run_dir(c.k);
    need(c.k.host().write_file(face_file_path(), ByteSpan((const uint8_t*)kDesk, 4)));   // host-io: the machine's own run dir (display state the desktop reads)
    c.o << "the desk is back on the display - the ledger, the authority, the rewind and the brief\n";
    return;
  }

  if (sub == "status" || sub == "list") {
    auto now = c.k.host().read_file(scene_file_path());   // host-io: the machine's own run dir (display state the desktop reads)
    std::string on = "beach";
    if (now.ok()) {
      on.assign((const char*)now->data(), now->size());
      const size_t b = on.find_first_not_of(" \t\r\n");
      const size_t e = (b == std::string::npos) ? b : on.find_first_of(" \t\r\n", b);
      on = (b == std::string::npos) ? std::string("beach")
                                    : on.substr(b, (e == std::string::npos) ? std::string::npos : e - b);
    }
    c.o << "On this machine's display: " << on << "\n";
    c.o << "  selected through " << scene_file_path() << ", which the compositor reads twice a second\n";
    {
      const uint64_t now2 = c.k.host().monotonic_ns() / 1000000ull;
      if (now2 < g_display_pinned_until_ms) {
        // The pin is now belt AND braces: a_t[2] is declined whether or not it
        // is running, because the choice is the operator's either way.
        c.o << "  asked for by an instruction " 
            << (3600000ull - (g_display_pinned_until_ms - now2)) / 60000ull
            << " minute(s) ago; the policy head cannot take the screen back.\n";
      } else {
        // Who owns the choice: the person. The policy head's a_t[2] is decoded
        // and printed but never applied, so nothing but an explicit request or
        // the saved desk state can move the scene.
        c.o << "  changed only by an explicit `display` request or the saved desk state\n";
      }
    }
    c.o << "What it knows how to draw:\n";
    std::vector<std::string> said;
    for (const auto& n : kScenes) {
      bool dup = false;
      for (const auto& s2 : said) if (s2 == n.what) dup = true;
      if (dup) continue;
      said.push_back(n.what);
      c.o << "  " << n.scene << "  " << n.what << "\n";
    }
    c.o << "  4  a scene composed on request, from a sky, a ground, a sun and a list of solids\n";
    c.o << "Say `display show <anything>`: the four above by name, and anything else gets composed.\n";
    c.o << "`display describe` prints the composed one. `display desk` brings the desk back.\n";
    return;
  }

  // Anything that is not a verb this command knows is treated as the NAME of
  // what to draw, so "show me a pyramid" works typed at the console exactly as
  // it works asked of the model. `show` is an alias for this command, and
  // aliasing it without this meant the sentence was dispatched here and refused
  // before the language model ever saw it.
  const bool verb = (sub == "show" || sub == "scene" || sub == "set");
  if (verb || sub != "status") {
    if (verb && a.size() < 3) fail(Status::Invalid, "display show <ocean|cyber|orb|pyramid>");
    std::string want;
    for (size_t i = verb ? 2 : 1; i < a.size(); ++i) {
      for (char ch : a[i]) want += (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
      if (i + 1 < a.size()) want += " ";
    }
    // A compiled-in scene wins only when the request IS that scene's name,
    // once the filler is taken out. It used to win on the name appearing
    // anywhere, so "three red pyramids at sunset" drew the one hardcoded
    // pyramid in the one hardcoded light and silently dropped the count, the
    // colour and the time of day — a worse answer than composing, and one that
    // looks like the machine ignoring what it was told. "pyramid" still gets
    // the hand-written scene, which is the better picture of exactly that.
    // What HOUR was asked for, before the words that name it are taken out of
    // the request. "the beach at dusk" is the compiled-in shoreline in a
    // different light, not a different scene: the sand, the wash, the surf line
    // and the palm are the things the person means by "my beach", and handing
    // the request to the composer throws all four away to honour the one word.
    bool hour_named = false;
    const float tod = userland::time_of_day_of(want, &hour_named);
    std::string bare;
    {
      static const char* kFiller[] = {"show", "me", "please", "display", "draw", "a", "an",
                                      "the", "of", "on", "with", "some", "put", "up", "ask",
                                      // The hour, and the words that carry it. They are read
                                      // above; leaving them in is what made "beach at dusk"
                                      // fail to match the beach it names.
                                      "at", "in", "under", "during", "midnight", "night",
                                      "starlit", "moonlit", "moonlight", "stars", "dawn",
                                      "sunrise", "daybreak", "morning", "midday", "noon",
                                      "daylight", "daytime", "afternoon", "golden", "hour",
                                      "sunset", "dusk", "twilight", "evening"};
      for (const auto& tok : split(want, ' ')) {
        if (tok.empty()) continue;
        bool filler = false;
        for (const char* f : kFiller) if (tok == f) { filler = true; break; }
        if (filler) continue;
        bare += (bare.empty() ? "" : " ") + tok;
      }
    }
    const Named* pick = nullptr;
    for (const auto& n : kScenes) {
      if (bare == n.name) { pick = &n; break; }
    }
    // Not a name it knows? Then COMPOSE one. Refusing here was the machine
    // saying it owned four pictures rather than that it could render: the
    // composer turns the request into a description — a sky, a ground, a sun
    // and a list of solids — and the shader draws whatever is in it. There is
    // no request this refuses.
    if (!pick) {
      auto composed = userland::compose_scene(want);
      const std::string text = composed.to_text();
      const std::string desc_path = scene_file_path() + ".composed";
      ensure_run_dir(c.k);
      need(c.k.host().write_file(desc_path, ByteSpan((const uint8_t*)text.data(), text.size())));   // host-io: the machine's own run dir (display state the desktop reads)
      // The description first, then the pointer to it: the other way round and
      // the compositor can read "composed" half a second before there is
      // anything to read.
      static const char kComposed[] = "composed";
      {
        // With a serial, so asking for the same scene twice is two requests.
        const std::string req = std::string(kComposed, 8) + "\n" +
                                std::to_string(c.k.host().monotonic_ns() / 1000000ull);
        need(c.k.host().write_file(scene_file_path(), ByteSpan((const uint8_t*)req.data(), req.size())));   // host-io: the machine's own run dir (display state the desktop reads)
      }
      {
        static const char kScene[] = "scene";
        (void)c.k.host().write_file(face_file_path(), ByteSpan((const uint8_t*)kScene, 5));   // host-io: the machine's own run dir (display state the desktop reads)
      }
      g_display_pinned_until_ms = c.k.host().monotonic_ns() / 1000000ull + 3600000ull;
      c.o << "this machine had no scene for \"" << want << "\", so it composed one:\n";
      c.o << "  " << composed.summary << "\n";
      c.o << "  " << composed.solids.size() << " solid(s), ground "
          << userland::ground_kind_name(composed.ground)
          << ", written to " << desc_path << "\n";
      c.o << "  scene 4 (composed); the compositor picks it up within half a second\n";
      c.o << "  `display describe` prints it, and it is a text file you can edit\n";
      return;
    }
    const std::string name = pick->name;
    ensure_run_dir(c.k);
    {
      // The name, and a serial beside it. The compositor applies the file when
      // the REQUEST changes, so without this `display show beach` on a machine
      // whose file already said beach changed nothing at all — which is exactly
      // what happened when the mind was asked to put the beach back and the
      // glass did not move.
      // The hour beside the name, in thousandths — one line of plain words and
      // an integer, which is what the rest of this file already is. The
      // compositor reads it back; a scene file written by hand can say the word
      // ("beach dusk") instead and gets the same picture.
      const std::string req = name + " " + std::to_string((int)(tod * 1000.0f + 0.5f)) + "\n" +
                              std::to_string(c.k.host().monotonic_ns() / 1000000ull);
      need(c.k.host().write_file(scene_file_path(), ByteSpan((const uint8_t*)req.data(), req.size())));   // host-io: the machine's own run dir (display state the desktop reads)
    }
    // And bring the face that is mostly background forward, so the thing asked
    // for is actually on the screen rather than behind the desk.
    {
      static const char kScene[] = "scene";
      (void)c.k.host().write_file(face_file_path(), ByteSpan((const uint8_t*)kScene, 5));   // host-io: the machine's own run dir (display state the desktop reads)
    }
    // Pinned for an hour, so the policy head exploring a_t[2] cannot take the
    // screen back off the person who just asked for something on it.
    g_display_pinned_until_ms = c.k.host().monotonic_ns() / 1000000ull + 3600000ull;
    c.o << "the display is now showing " << pick->what;
    if (hour_named && pick->scene == 0) {
      c.o << ", at " << (tod < 0.12f ? "midnight" : tod < 0.29f ? "dawn"
                      : tod < 0.42f ? "morning"  : tod < 0.60f ? "midday"
                      : tod < 0.86f ? "dusk"     : "midnight");
    }
    c.o << "\n";
    c.o << "  scene " << pick->scene << " (" << name << "), written to " << scene_file_path() << "\n";
    c.o << "  the compositor picks it up within half a second; nothing was restarted\n";
    return;
  }

  fail(Status::Invalid, "display status | show <ocean|cyber|orb|pyramid>");
}

void cmd_jepa(Ctx& c, const Args& a) {
  std::string sub = a.size() > 1 ? a[1] : "status";
  // Bound by default, because it now only TAKES the products it wins on and
  // declines the rest back to the CPU. `jepa gpu off` unbinds it, and
  // `jepa bench` is what decided the threshold.
  static bool bound_once = false;
  if (!bound_once && sub != "gpu") { bound_once = true; bind_predictor_offload(c, true); }
  // And the levers. Without this the recorder chose an action every step and
  // nothing consumed it: `jepa status` said "Actuation: NOT BOUND" on a machine
  // that owns two of the four levers outright.
  bind_substrate_actuation(c);
  auto* model = intelligence::get_global_jepa_world_model();
  auto* policy = intelligence::get_global_action_policy_head();
  auto* logger = intelligence::get_global_trace_logger();

  // Where this machine keeps what it has learned. Beside the kernel's other
  // durable state, so a checkpoint travels with the root it belongs to rather
  // than with whatever directory a command happened to be run from.
  const std::string ckpt_dir = c.k.config().root;
  const std::string ckpt_predictor = ckpt_dir + "/jepa.predictor";
  const std::string ckpt_policy = ckpt_dir + "/jepa.policy";
  const std::string ckpt_traces = ckpt_dir + "/jepa.traces";

  // Persisting is two files because they are two models with two lifetimes:
  // the predictor can be right about this machine while the policy is still
  // wrong about what to do with it, and reloading one must not require the
  // other. Returns what it wrote so the caller can say so instead of assuming.
  auto save_checkpoints = [&](std::string& note) -> bool {
    auto mb = model->serialize();
    auto pb = policy->serialize();
    auto tb = logger->serialize();
    auto w1 = c.k.host().write_file(ckpt_predictor, ByteSpan(mb.data(), mb.size()));   // host-io: the world model's checkpoint under the kernel root
    auto w2 = c.k.host().write_file(ckpt_policy, ByteSpan(pb.data(), pb.size()));   // host-io: the world model's checkpoint under the kernel root
    auto w3 = c.k.host().write_file(ckpt_traces, ByteSpan(tb.data(), tb.size()));   // host-io: the world model's checkpoint under the kernel root
    if (!w1.ok() || !w2.ok() || !w3.ok()) {
      note = "could not write checkpoint under " + ckpt_dir;
      return false;
    }
    note = std::to_string(mb.size()) + " B predictor + " + std::to_string(pb.size()) + " B policy + "
         + std::to_string(tb.size()) + " B traces (" + std::to_string(logger->size()) + " transition(s))";
    return true;
  };

  // A missing checkpoint is not an error — a machine that has never trained has
  // nothing to restore. A checkpoint that EXISTS and will not load is an error
  // worth printing: it means this machine is quietly starting from scratch
  // while its operator believes it is carrying on.
  auto load_checkpoints = [&](std::string& note) -> bool {
    bool any = false;
    std::string problems;
    auto f1 = c.k.host().read_file(ckpt_predictor);   // host-io: the world model's checkpoint under the kernel root
    if (f1.ok()) {
      std::string why;
      if (model->deserialize(*f1, &why)) any = true;
      else problems += " predictor: " + why + ";";
    }
    auto f2 = c.k.host().read_file(ckpt_policy);   // host-io: the world model's checkpoint under the kernel root
    if (f2.ok()) {
      std::string why;
      if (policy->deserialize(*f2, &why)) any = true;
      else problems += " policy: " + why + ";";
    }
    auto f3 = c.k.host().read_file(ckpt_traces);   // host-io: the world model's checkpoint under the kernel root
    if (f3.ok()) {
      std::string why;
      if (logger->deserialize(*f3, &why)) any = true;
      else problems += " traces: " + why + ";";
    }
    if (!problems.empty()) { note = "REFUSED —" + problems; return false; }
    const size_t measured = logger->measured_count();
    note = any ? ("predictor at " + std::to_string(model->total_steps()) + " step(s), policy at "
                  + std::to_string(policy->total_steps()) + " step(s), "
                  + std::to_string(logger->size()) + " transition(s): " + std::to_string(measured)
                  + " recorded, " + std::to_string(logger->size() - measured) + " synthetic")
               : "no checkpoint yet";
    return true;
  };

  if (sub == "gpu") {
    const bool on = a.size() < 3 || a[2] == "on" || a[2] == "1";
    bind_predictor_offload(c, on);
    if (on && !intelligence::matmul_offload_is_live()) {
      c.o << "no device on this machine says it can do a matmul; the predictor stays on the CPU\n";
      return;
    }
    c.o << "the predictor's matrix products now run on: " << intelligence::matmul_offload_name() << "\n";
    return;
  }

  if (sub == "bench") {
    // The only claim worth making about an accelerator is a measured one, and
    // the honest form of it is the same work done both ways, back to back.
    const int steps = a.size() > 2 ? std::max(1, std::atoi(a[2].c_str())) : 5;
    const int bsize = a.size() > 3 ? std::max(1, std::atoi(a[3].c_str())) : 32;
    if (logger->size() < (size_t)bsize) {
      c.o << "only " << logger->size() << " trace sample(s) logged; run `jepa record count="
          << (bsize * 2) << "` first so this times real transitions\n";
      return;
    }
    auto batch_of = [&]() {
      std::vector<intelligence::TraceSample> b;
      const auto all = logger->get_samples((size_t)bsize);
      for (const auto& s2 : all) b.push_back(s2);
      return b;
    };
    auto time_it = [&](bool gpu) {
      g_mm_take_everything = gpu;
      bind_predictor_offload(c, gpu);
      if (gpu && !intelligence::matmul_offload_is_live()) return -1.0;
      const auto before = intelligence::matmul_offload_counters();
      const uint64_t t0 = c.k.host().monotonic_ns();
      float loss = 0.0f;
      for (int i = 0; i < steps; ++i) {
        auto b = batch_of();
        auto r = model->train_step(b);
        if (r.ok()) loss = *r;
      }
      const uint64_t t1 = c.k.host().monotonic_ns();
      const auto after = intelligence::matmul_offload_counters();
      c.o << "  " << (gpu ? "device" : "cpu   ") << ": " << steps << " step(s) of batch " << bsize
          << " in " << ((double)(t1 - t0) / 1e6) << " ms  ("
          << ((double)(t1 - t0) / 1e6 / (double)steps) << " ms/step), last loss " << loss
          << ", products offloaded " << (after.offloaded - before.offloaded)
          << ", declined " << (after.declined - before.declined) << "\n";
      return (double)(t1 - t0) / 1e6 / (double)steps;
    };
    c.o << "The predictor's five matrix products per step, both ways:\n";
    const double cpu_ms = time_it(false);
    const double gpu_ms = time_it(true);
    if (gpu_ms < 0.0) {
      c.o << "no device took them, so there is nothing to compare\n";
    } else if (gpu_ms > 0.0) {
      const double ratio = cpu_ms / gpu_ms;
      c.o << (ratio > 1.05 ? "the device is faster: " : ratio < 0.95 ? "the DEVICE IS SLOWER: " : "it is a wash: ")
          << ratio << "x (" << cpu_ms << " ms/step on the cpu, " << gpu_ms << " ms/step on the device)\n";
      if (ratio < 1.05) {
        c.o << "at this size the operands cost more to move than the arithmetic costs to do;\n"
            << "the CPU path stays the default and this is why\n";
      }
    }
    g_mm_take_everything = false;
    bind_predictor_offload(c, true);
    return;
  }


  if (sub == "sweep") {
    // Does the instrument the REWARD reads actually respond to the lever?
    //
    // `jepa prove` answers a different question: it reads the kernel's own
    // switch counter around a fixed workload and shows 359 -> 24. The reward
    // does not read that counter — it reads state dimension 2, the switch rate
    // the telemetry provider reports over the settled interval — and if THAT
    // does not move with the action then no amount of recorded data and no
    // number of gradient steps can teach the lever, because the return is a
    // constant. Training on it would show exactly what the metal run showed:
    // a reward that does not improve, and no way to tell whether the policy or
    // the instrument was at fault.
    //
    // So: pin a_t[0] at each end of its range, settle, and print what the
    // reward's own instrument said.
    if (!intelligence::telemetry_is_live()) {
      c.o << "no telemetry provider: there is no instrument to sweep\n";
      return;
    }
    // The workload is held FIXED across the sweep and every level is repeated,
    // or the thing being measured is the settler's varying pressure and the
    // noise in it rather than the lever. A single pass per level reported a
    // spread of 1.15 on a host where a_t[0] is declined by name — which is
    // noise wearing a result's clothes.
    const int repeats = a.size() > 2 ? std::max(1, std::atoi(a[2].c_str())) : 3;
    intelligence::set_settle_pressure(1);
    c.o << "Sweeping a_t[0] with the workload held fixed, " << repeats << " repeat(s) each:\n";
    c.o << "  a_t[0]  asked  HELD  sink  cpu[0]  switch[2]  loss[6]  load[7]  reward (mean +- half-range)\n";
    const float ends[] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
    float lo = 1e30f, hi = -1e30f, worst_noise = 0.0f;
    for (float v : ends) {
      float sum = 0.0f, rlo = 1e30f, rhi = -1e30f;
      float c0 = 0.0f, c2 = 0.0f, c6 = 0.0f, c7 = 0.0f;
      uint64_t sw = 0, tk = 0;
      uint32_t held = 0;
      bool took = false;
      uint32_t slice = 0;
      bool settled = false;
      for (int rep = 0; rep < repeats; ++rep) {
        std::vector<float> a_t(4, 0.0f);
        a_t[0] = v;
        const auto act = intelligence::decode_action(a_t);
        slice = act.scheduler_slice_ticks;
        const auto s_t = intelligence::SubstrateState::sample_kernel_telemetry();
        intelligence::SubstrateActuation landed_as;
        took = intelligence::apply_action(a_t, &landed_as);
        settled = intelligence::substrate_settle() || settled;
        const auto s_t1 = intelligence::SubstrateState::sample_kernel_telemetry();
        const float r = intelligence::measured_reward(s_t, s_t1);
        sum += r;
        if (r < rlo) rlo = r;
        if (r > rhi) rhi = r;
        c0 += s_t1.vector[0]; c2 += s_t1.vector[2]; c6 += s_t1.vector[6]; c7 += s_t1.vector[7];
        // NOT a fresh read: the provider differences against its own previous
        // call, so reading it again here restarts the interval and the rate
        // comes back 0 — which is exactly what this line printed before.
        {
          // Taken from the state's own timestamped read, not a fresh one.
          const auto probe = intelligence::last_substrate_telemetry();
          sw += probe.switch_events;
          tk += probe.interval_ticks;
          held = probe.scheduler_slice_now;
        }
      }
      const float inv = 1.0f / (float)repeats;
      const float mean = sum * inv;
      const float half = (rhi - rlo) * 0.5f;
      if (half > worst_noise) worst_noise = half;
      if (mean < lo) lo = mean;
      if (mean > hi) hi = mean;
      c.o << "  " << v << "     " << slice << "     " << held << "    " << (took ? "took" : "NONE")
          << "  " << (c0 * inv) << "   " << (c2 * inv)
          << " (" << (sw / (repeats ? repeats : 1)) << " switch(es) over "
          << (tk / (repeats ? repeats : 1)) << " tick(s))"
          << "     " << (c6 * inv) << "      " << (c7 * inv) << "     " << mean << " +- " << half
          << (settled ? "" : "   (NO SETTLER: idle machine)") << "\n";
    }
    const float spread = hi - lo;
    c.o << "noise within a single setting (worst half-range): " << worst_noise << "\n";
    if (spread <= worst_noise * 1.5f) {
      c.o << "the spread across the lever (" << spread << ") is NOT LARGER than the noise\n"
          << "within one setting (" << worst_noise << "). Whatever the reward is doing here,\n"
          << "it is not responding to this lever, and a policy gradient would be climbing\n"
          << "sampling noise. Reported as a failure to distinguish, not as a response.\n";
      return;
    }
    c.o << "reward spread across the lever's whole range: " << spread << "\n";
    if (spread < 0.05f) {
      c.o << "THE REWARD DOES NOT RESPOND TO THIS LEVER on this substrate.\n"
          << "A policy gradient has nothing to ascend: the return is the same at both\n"
          << "ends of the action. Either the lever is not applied here (hosted, the\n"
          << "scheduler belongs to Linux and a_t[0] is declined by name), or the\n"
          << "instrument is saturated. Training against this would produce a number\n"
          << "that does not move and no way to tell which.\n";
    } else {
      c.o << "the reward responds: there is a gradient here for the policy to climb\n";
    }
    return;
  }

  if (sub == "status") {
    c.o << "JEPA Substrate World Model Status:\n";
    c.o << "Latent State Dimension: " << intelligence::kSubstrateStateDim << " (R^1024)\n";
    c.o << "Action Vector Dimension: " << intelligence::kActionVectorDim << " (R^128)\n";
    const size_t measured = logger->measured_count();
    c.o << "Predictor Training Steps: " << model->total_steps() << "\n";
    if (model->total_steps() == 0) c.o << "Current Latent Prediction Loss (MSE): not trained yet\n";
    else c.o << "Current Latent Prediction Loss (MSE): " << model->current_loss() << "\n";
    if (policy->total_steps() == 0) {
      c.o << "Action Policy Surrogate Objective (-A*log pi): not trained yet\n";
    } else {
      c.o << "Action Policy Surrogate Objective (-A*log pi): " << policy->current_loss() << "\n";
      c.o << "Mean Return of the last batch (switch cost + 4x packet loss + 2x load over saturation, negated): "
          << policy->current_return()
          << (measured == logger->size() ? " (measured)" : measured == 0 ? " (SYNTHETIC traces, not measured)"
                                                                         : " (a mix of measured and synthetic traces)")
          << "\n";
      c.o << "Policy Return Baseline: " << policy->current_baseline() << "\n";
    }
    c.o << "Action Policy Steps: " << policy->total_steps() << "\n";
    c.o << "Logged Execution Trace Samples: " << logger->size() << " (" << measured << " recorded, "
        << (logger->size() - measured) << " synthetic)\n";
    {
      auto* region = intelligence::get_global_shared_state_region();
      c.o << "Shared State Region Sequence: " << (region ? region->sequence.load() : 0)
          << (region && region->sequence.load() ? " (live)" : " (never published)") << "\n";
    }
    // What this actually ran on, asked of the core rather than written here.
    // These two lines used to be constants that said "not wired" whatever was
    // true, which is the same defect as any other status surface printing a
    // number nobody measured.
    c.o << "Predictor Backend: " << intelligence::matmul_offload_name() << "\n";
    {
      const auto n = intelligence::matmul_offload_counters();
      c.o << "GPU Offload: " << (intelligence::matmul_offload_is_live() ? "registered" : "not registered")
          << " - " << n.offloaded << " product(s) offloaded, " << n.declined << " declined, "
          << (n.macs / 1000000ull) << "M multiply-accumulates on the device\n";
      c.o << "  (`jepa gpu on|off` binds it; `jepa bench [steps] [batch]` times both)\n";
    }
    {
      auto t = intelligence::read_substrate_telemetry();
      c.o << "Telemetry: " << (intelligence::telemetry_is_live() ? "LIVE (substrate provider registered)"
                                                                 : "SYNTHETIC (no provider — values are a sine driver)") << "\n";
      c.o << "  s_t[0] cpu load        " << t.cpu_load << "\n";
      c.o << "  s_t[1] nic rx depth    " << t.nic_ring_depth << "\n";
      c.o << "  s_t[2] ctx switch rate " << t.ctx_switch_rate << "\n";
      c.o << "  s_t[3] heap pressure   " << t.mem_pressure << "\n";
      c.o << "  s_t[4] compute occ.    " << t.compute_occupancy << "\n";
      c.o << "  s_t[6] packet loss     " << t.packet_loss
          << "  (interface drops + udp receive-buffer overflow, this machine's own counters)\n";
      c.o << "  s_t[7] load imbalance  " << t.load_imbalance
          << "  (runnable processes per processor, 1.0 = four times oversubscribed)\n";
      if (intelligence::pressure_hook_is_live()) {
        c.o << "  conditions             : generated, swept 0..3 while recording "
            << "(idle, matched, contended, thrashing with traffic)\n";
      } else {
        c.o << "  conditions             : NOT generated - one fixed workload, so the state\n"
            << "                           space the policy sees is one slice of itself\n";
      }
      auto a_now = policy->select_action(intelligence::SubstrateState::sample_kernel_telemetry());
      auto decoded = intelligence::decode_action(a_now);
      // Four levers, and which of them the sink bound in THIS process takes —
      // asked of the sink, not assumed: hosted it is the quota alone; bare
      // metal's metal_actuate takes the slice, the scene and the NIC window.
      const auto own = lever_ownership();
      c.o << "Actuation: " << (intelligence::actuation_is_live() ? "LIVE" : "NOT BOUND (nothing consumes a_t)")
          << " — sink: " << own.sink << "\n";
      c.o << "  a_t[0] -> scheduler slice  " << decoded.scheduler_slice_ticks << " tick(s)  ["
          << lever_word(own.slice) << (own.slice == 0 && g_act_kernel ? ": Linux owns the timer" : "") << "]\n";
      c.o << "  a_t[1] -> nic refill window " << decoded.nic_refill_window
          << " buffer(s) posted, prefetch " << (decoded.nic_prefetch_enabled ? "on" : "off") << "  ["
          << lever_word(own.nic) << (own.nic == 0 && g_act_kernel ? ": Linux owns the receive ring" : "") << "]\n";
      c.o << "  a_t[2] -> display scene    " << decoded.display_scene << " (" << scene_name(decoded.display_scene)
          << ")  [" << lever_word(own.display)
          << (own.display == 0 && g_act_kernel ? ": only an explicit `display` request writes " + scene_file_path() : std::string())
          << "]\n";
      c.o << "  a_t[3] -> compute quota    " << decoded.compute_quota_pct << "% of measured device time per period  ["
          << lever_word(own.quota) << "]\n";
      {
        const auto rep = actuation_report();
        c.o << "  pulled so far: " << rep.quota_writes << " quota assignment(s); "
            << rep.display_declined << " display change(s) asked for and DECLINED\n";
        c.o << "  a_t[4..127] are still latent capacity nothing reads, and that is still true\n";
      }
    }
  } else if (sub == "synthesize" || sub == "gen") {
    size_t count = 500;
    if (has_kv(a, "count")) count = to_u64(kv(a, "count"));
    else if (a.size() > 2) count = to_u64(a[2]);

    size_t n = intelligence::generate_synthetic_traces(*logger, count);
    c.o << "[JEPA TRACE SYNTHESIZER] WARNING: synthetic, not measured — use `jepa record` on metal.\nGenerated " << n << " synthetic operational state transition triples (s_t, a_t, s_{t+1}).\n";
    c.o << "Total Logged Execution Trace Samples: " << logger->size() << "\n";
  } else if (sub == "daemon") {
    // The learning loop, run to a bound the caller sets. `daemon` used to be a
    // second spelling of `train`: the same 50 bounded steps over one frozen
    // batch of SYNTHETIC traces, after which the process exited — 1.9 seconds,
    // nothing recorded, nothing kept. This records a real transition and takes
    // a real step, over and over, and checkpoints as it goes so stopping it at
    // any point loses at most one checkpoint interval.
    uint64_t iters = 200;
    if (has_kv(a, "iters")) iters = to_u64(kv(a, "iters"));
    else if (a.size() > 2) iters = to_u64(a[2]);
    size_t batch_size = has_kv(a, "batch") ? (size_t)to_u64(kv(a, "batch")) : 64;
    uint64_t every = has_kv(a, "checkpoint") ? to_u64(kv(a, "checkpoint")) : 50;
    if (every == 0) every = 1;

    uint64_t ran = 0;
    float last_reward = 0.0f, first_reward = 0.0f;
    intelligence::LearnStep st;
    for (uint64_t i = 0; i < iters; ++i) {
      st = intelligence::learn_once(batch_size);
      if (!st.ran) break;
      if (ran == 0) first_reward = st.reward;
      last_reward = st.reward;
      ++ran;
      if (ran % every == 0) { std::string n; (void)save_checkpoints(n); }
    }
    if (!st.ran && ran == 0) {
      c.o << "[JEPA DAEMON] refusing: " << (st.refused ? st.refused : "unavailable") << ".\n"
          << "This machine cannot learn from itself — it would be fitting the sine driver.\n"
          << "Run it on the bare-metal machine, where telemetry and the settler are both live.\n";
      return;
    }
    std::string note;
    const bool saved = save_checkpoints(note);
    c.o << "[JEPA DAEMON] " << ran << " live learning iteration(s) — record, act, settle, score, step.\n";
    c.o << "Measured reward: first " << first_reward << " -> last " << last_reward << "\n";
    c.o << "World Model Prediction Loss (MSE): " << st.world_loss << "\n";
    c.o << "Action Policy Surrogate Objective (-A*log pi): " << st.policy_loss << "\n";
    c.o << "Policy Return Baseline: " << policy->current_baseline() << "\n";
    if (lever_ownership().slice > 0)
      c.o << "Scheduler slice the last action set: " << st.slice_applied << " tick(s)\n";
    else
      c.o << "Scheduler slice: the last action asked for " << st.slice_applied << " tick(s); "
          << lever_word(lever_ownership().slice) << " (sink: " << lever_ownership().sink << ")\n";
    c.o << "Logged Execution Trace Samples: " << logger->size() << "\n";
    c.o << (saved ? "Checkpoint written: " : "Checkpoint FAILED: ") << note << "\n";
    c.o << "Total Cumulative World Steps: " << model->total_steps() << "\n";
    c.o << "Total Cumulative Policy Steps: " << policy->total_steps() << "\n";
  } else if (sub == "train") {
    uint64_t steps = 50;
    if (has_kv(a, "steps")) steps = to_u64(kv(a, "steps"));
    else if (a.size() > 2) steps = to_u64(a[2]);

    size_t batch_size = 64;
    if (has_kv(a, "batch")) batch_size = to_u64(kv(a, "batch"));

    bool synthesized_now = false;
    if (logger->size() == 0) {
      intelligence::generate_synthetic_traces(*logger, 500);
      synthesized_now = true;
    }
    const size_t measured = logger->measured_count();

    // Publishing is what makes the region readable from userland (SYS_JEPA_SHM,
    // voice shm). Training with nothing published leaves the region at seq 0.
    auto publish_cycle = [&]() {
      auto s_t = intelligence::SubstrateState::sample_kernel_telemetry();
      auto a_t = policy->select_action(s_t);
      auto s_hat = model->predict_next_state(s_t, a_t);
      intelligence::update_shared_state_region(s_t, a_t, s_hat);
    };

    float world_loss = 0.0f;
    float pol_loss = 0.0f;
    const auto offload_before = intelligence::matmul_offload_counters();

    for (uint64_t i = 0; i < steps; ++i) {
      // Resampled EVERY step. Hoisting this out of the loop meant all N steps
      // saw one frozen batch, which is why 500 steps moved the loss no further
      // than 50 did — 0.00107596 against 0.0010759, a difference in the eighth
      // decimal. The logger holds more than one batch; a step should see a
      // different draw of it.
      auto samples = logger->get_samples(batch_size);
      auto res_w = model->train_step(samples);
      if (res_w.ok()) world_loss = *res_w;
      auto res_p = policy->train_step(samples);
      if (res_p.ok()) pol_loss = *res_p;
      publish_cycle();   // every step lands in the shared region userland reads
    }
    // Training that is not written down is training this machine forgets at
    // exit. Saved here, unconditionally, because a run that ends any other way
    // than by choice still earned its steps.
    std::string save_note;
    const bool saved = save_checkpoints(save_note);
    auto* pub_region = intelligence::get_global_shared_state_region();
    const auto offload_after = intelligence::matmul_offload_counters();
    const uint64_t moved = offload_after.offloaded - offload_before.offloaded;
    c.o << "[JEPA TRAIN] Completed " << steps << " backpropagation step(s): ";
    if (moved)
      c.o << moved << " of the predictor's matrix products ran on " << intelligence::matmul_offload_name()
          << " (" << (offload_after.declined - offload_before.declined) << " declined back to the CPU); "
          << "everything else on the CPU.\n";
    else
      c.o << "all on the CPU (no matrix product was taken by an offload).\n";
    if (synthesized_now)
      c.o << "Traces: none were logged, so 500 SYNTHETIC ones were generated to train on (not this machine's data)\n";
    else if (measured < logger->size())
      c.o << "Traces: " << logger->size() << " (" << measured << " recorded, " << (logger->size() - measured)
          << " synthetic)\n";
    c.o << (saved ? "Checkpoint written: " : "Checkpoint FAILED: ") << save_note << "\n";
    c.o << "Shared State Region Sequence: " << (pub_region ? pub_region->sequence.load() : 0) << "\n";
    c.o << "World Model Prediction Loss (MSE): " << world_loss << "\n";
    // The policy head is trained by REINFORCE against the measured reward, so
    // what moves here is the surrogate objective and the mean return — an MSE
    // against its own action would sit at 0 and mean nothing.
    c.o << "Action Policy Surrogate Objective (-A*log pi): " << pol_loss << "\n";
    c.o << (measured == logger->size() ? "Mean Measured Return" : measured == 0 ? "Mean Return over SYNTHETIC traces (not measured)"
                                                                                  : "Mean Return over mixed measured/synthetic traces")
        << " (switch cost + 4x packet loss + 2x load over saturation, negated): " << policy->current_return() << "\n";
    c.o << "Policy Return Baseline: " << policy->current_baseline()
        << "   exploration sigma: " << policy->exploration_sigma() << "\n";
    c.o << "Total Cumulative World Steps: " << model->total_steps() << "\n";
    c.o << "Total Cumulative Policy Steps: " << policy->total_steps() << "\n";
  } else if (sub == "record") {
    // Record REAL transitions: read the machine, act on it, read it again, and
    // log what actually followed. This is the trace the synthesizer was standing
    // in for. Training on these is the only way the model learns THIS machine
    // rather than the generator's assumptions.
    uint64_t n = 100;
    if (has_kv(a, "count")) n = to_u64(kv(a, "count"));
    else if (a.size() > 2) n = to_u64(a[2]);
    if (!intelligence::telemetry_is_live()) {
      c.o << "[JEPA RECORD] refusing: no substrate telemetry provider is registered, so every\n"
             "sample would be the synthetic driver. Run this on the bare-metal machine, or\n"
             "register a provider first. `jepa synthesize` is the honest name for that.\n";
      return;
    }
    size_t before = logger->size();
    float reward_sum = 0.0f;
    float best_reward = -1e30f;
    bool settled = false;
    float visited_cpu = 0.0f, visited_switch = 0.0f, visited_loss = 0.0f, visited_load = 0.0f;
    float loss_sum = 0.0f, load_sum = 0.0f;
    uint64_t with_loss = 0, oversubscribed = 0;
    // The pressure the machine is put under is SWEPT, not fixed. With one
    // workload every transition looks alike: the state space the policy sees is
    // one thin slice of itself, and the instruments that were added beside the
    // switch counter — packet loss and runnable work per processor — never move,
    // so nothing scored on them can be learned. Four levels, round-robin, so a
    // recording covers an idle machine, a matched one, a contended one and a
    // thrashing one with real traffic being dropped in it.
    const int pressure_levels = has_kv(a, "pressure") ? (int)to_u64(kv(a, "pressure")) : -1;
    for (uint64_t i = 0; i < n; ++i) {
      if (pressure_levels >= 0) intelligence::set_settle_pressure(pressure_levels);
      else intelligence::set_settle_pressure((int)(i % 4));
      auto s_t = intelligence::SubstrateState::sample_kernel_telemetry();
      auto a_t = policy->select_action(s_t);
      intelligence::apply_action(a_t);                       // the action really happens
      // ...and then the machine has to actually RUN under it. Without this the
      // two reads are microseconds apart on an idle box, every counter delta is
      // 0, and the reward is structurally 0 no matter what the policy chose.
      settled = intelligence::substrate_settle() || settled;
      auto s_t1 = intelligence::SubstrateState::sample_kernel_telemetry();   // what followed
      // Score the transition with the SAME instrument `host jepa prove` reads:
      // context switches at equal work completed. This measured number is what
      // the policy head is trained against — not its own output.
      float reward = intelligence::measured_reward(s_t, s_t1);
      // What the machine was actually LIKE at each transition. Generating
      // conditions is only worth anything if they were visited, and the way to
      // know is to report the space the recording covered rather than the
      // intention behind it.
      if (s_t1.vector.size() > 7) {
        visited_cpu = std::max(visited_cpu, s_t1.vector[0]);
        visited_switch = std::max(visited_switch, s_t1.vector[2]);
        visited_loss = std::max(visited_loss, s_t1.vector[6]);
        visited_load = std::max(visited_load, s_t1.vector[7]);
        loss_sum += s_t1.vector[6];
        load_sum += s_t1.vector[7];
        if (s_t1.vector[6] > 0.0f) ++with_loss;
        if (s_t1.vector[7] > 0.25f) ++oversubscribed;
      }
      reward_sum += reward;
      if (reward > best_reward) best_reward = reward;
      logger->log_transition(s_t, a_t, s_t1, reward);
      intelligence::update_shared_state_region(s_t, a_t, s_t1);
    }
    c.o << "[JEPA RECORD] Logged " << (logger->size() - before)
        << " MEASURED (s_t, a_t, s_t+1) transition(s) from this machine.\n";
    c.o << "Trace buffer: " << logger->size() << " sample(s)"
        << " (source: live substrate telemetry, not the synthesizer)\n";
    c.o << "Measured reward (switch cost + 4x packet loss + 2x load over saturation, negated): mean "
        << (n ? reward_sum / static_cast<float>(n) : 0.0f)
        << ", best " << (n ? best_reward : 0.0f) << "\n";
    // The state space this recording actually covered. Generating conditions is
    // worth nothing unless they were VISITED, and the honest way to know is to
    // report the machine as it was at each transition rather than the workload
    // that was intended.
    c.o << "State space visited (peak over " << n << " transition(s)):\n";
    c.o << "  cpu load        up to " << visited_cpu << "\n";
    c.o << "  switch rate     up to " << visited_switch << "\n";
    c.o << "  packet loss     up to " << visited_loss << "   ("
        << with_loss << " of " << n << " transition(s) had loss in them, mean "
        << (n ? loss_sum / (float)n : 0.0f) << ")\n";
    c.o << "  load imbalance  up to " << visited_load << "   ("
        << oversubscribed << " of " << n << " past one runnable process per processor, mean "
        << (n ? load_sum / (float)n : 0.0f) << ")\n";
    if (with_loss == 0 && intelligence::pressure_hook_is_live()) {
      c.o << "  NO loss was seen. The reward's loss term cannot teach anything from this\n"
             "  recording, and saying so beats training on it and calling the result learning.\n";
    }
    if (!settled) {
      c.o << "NOTE: no workload settler is registered on this substrate, so each\n"
             "transition was measured on an idle machine microseconds after the\n"
             "action. The rewards above are 0 by construction, not by result.\n";
    }
  } else if (sub == "tick") {
    uint64_t n = 1;
    if (has_kv(a, "count")) n = to_u64(kv(a, "count"));
    else if (a.size() > 2) n = to_u64(a[2]);
    intelligence::SubstrateState s_t;
    std::vector<float> a_t;
    intelligence::SubstrateState s_hat;
    intelligence::SubstrateActuation applied;
    bool applied_ok = false;
    for (uint64_t i = 0; i < n; ++i) {
      s_t = intelligence::SubstrateState::sample_kernel_telemetry();
      a_t = policy->select_action(s_t);
      s_hat = model->predict_next_state(s_t, a_t);
      intelligence::update_shared_state_region(s_t, a_t, s_hat);
      applied_ok = intelligence::apply_action(a_t, &applied);   // the lever, for real
    }
    auto* region = intelligence::get_global_shared_state_region();
    c.o << "[JEPA TICK] Ran " << n << " perceive -> policy -> ACT -> predict -> publish cycle(s).\n";
    c.o << "Telemetry: " << (intelligence::telemetry_is_live() ? "live" : "synthetic")
        << "   Actuation: " << (applied_ok ? "applied to the substrate" : "no sink bound") << "\n";
    if (lever_ownership().slice > 0)
      c.o << "Scheduler slice now: " << applied.scheduler_slice_ticks << " tick(s)\n";
    else
      c.o << "Scheduler slice: a_t[0] asked for " << applied.scheduler_slice_ticks << " tick(s); "
          << lever_word(lever_ownership().slice) << " (sink: " << lever_ownership().sink << ")\n";
    c.o << "Shared State Region Sequence: " << (region ? region->sequence.load() : 0) << "\n";
    c.o << "s_t[0..4]   cpu=" << s_t.vector[0] << " nic=" << s_t.vector[1]
        << " ctx=" << s_t.vector[2] << " mem=" << s_t.vector[3] << "\n";
    c.o << "a_t[0..3]   " << a_t[0] << " " << a_t[1] << " " << a_t[2] << " " << a_t[3] << "\n";
    c.o << "s_hat[0..3] " << s_hat.vector[0] << " " << s_hat.vector[1] << " "
        << s_hat.vector[2] << " " << s_hat.vector[3] << "\n";
  } else if (sub == "predict") {
    intelligence::SubstrateState s_t;
    s_t.vector.resize(intelligence::kSubstrateStateDim, 0.1f);
    std::vector<float> a_t = policy->select_action(s_t);
    intelligence::SubstrateState s_hat = model->predict_next_state(s_t, a_t);
    c.o << "[JEPA LATENT PREDICTION] Input s_t (R^1024) + ActionPolicy pi_psi(s_t) -> Predicted s_hat_{t+1} (R^1024)\n";
    c.o << "Predicted Latent Vector First 4 Components: ["
        << s_hat.vector[0] << ", " << s_hat.vector[1] << ", "
        << s_hat.vector[2] << ", " << s_hat.vector[3] << "...]\n";
  } else if (sub == "policy") {
    intelligence::SubstrateState s_t;
    s_t.vector.resize(intelligence::kSubstrateStateDim, 0.1f);
    std::vector<float> a_t = policy->select_action(s_t);
    c.o << "[JEPA ACTION POLICY] Input s_t (R^1024) -> Emitted Action Vector a_t (R^128)\n";
    c.o << "Action Vector First 4 Components: ["
        << a_t[0] << ", " << a_t[1] << ", "
        << a_t[2] << ", " << a_t[3] << "...]\n";
  } else if (sub == "shm" || sub == "region") {
    auto* region = intelligence::get_global_shared_state_region();
    c.o << "[JEPA SHARED MEMORY STATE REGION]\n";
    c.o << "Atomic Sequence Counter: " << region->sequence.load() << "\n";
    c.o << "State Vector Dimension: " << intelligence::kSubstrateStateDim << " floats (mapped)\n";
    c.o << "Action Vector Dimension: " << intelligence::kActionVectorDim << " floats (mapped)\n";
    c.o << "Predicted Next State Dimension: " << intelligence::kSubstrateStateDim << " floats (mapped)\n";
  } else if (sub == "latency") {
    // (Was `bench || latency`; `bench` is the CPU-vs-device timing above and
    // always took that branch, so this one is `latency`.)
    size_t iterations = 1000;
    if (has_kv(a, "count")) iterations = to_u64(kv(a, "count"));
    else if (a.size() > 2) iterations = to_u64(a[2]);

    intelligence::SubstrateState s_t;
    s_t.vector.resize(intelligence::kSubstrateStateDim, 0.05f);
    std::vector<float> a_t(intelligence::kActionVectorDim, 0.02f);
    
    auto t0 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
      auto s_hat = model->predict_next_state(s_t, a_t);
      (void)s_hat;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double total_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    double avg_us = total_us / static_cast<double>(iterations);
    double ops_sec = (static_cast<double>(iterations) / total_us) * 1e6;

    c.o << "[JEPA SYS_JEPA_PREDICT BENCHMARK]\n";
    c.o << "Total Iterations: " << iterations << " calls\n";
    c.o << "Total Execution Time: " << (total_us / 1000.0) << " ms\n";
    c.o << "Mean Latency per Call: " << avg_us << " us (" << (avg_us / 1000.0) << " ms)\n";
    c.o << "Predictor Throughput: " << ops_sec << " ops/sec\n";
    // A verdict from the number just measured, not a constant.
    c.o << "Verdict: mean " << (avg_us / 1000.0) << " ms per prediction is "
        << (avg_us < 100.0 ? "UNDER" : "NOT under") << " 0.1 ms on this machine\n";
  } else if (sub == "template" || sub == "layout" || sub == "ui") {
    intelligence::SubstrateState s_t;
    s_t.vector.resize(intelligence::kSubstrateStateDim, 0.1f);
    
    std::vector<float> a_ui(intelligence::kActionVectorDim, 0.0f);
    a_ui[0] = 1900.0f; // Width
    a_ui[1] = 1200.0f; // Height
    a_ui[2] = 0.85f;   // Telemetry Widget Opacity
    a_ui[3] = 144.0f;  // Dynamic Refresh Rate (Hz)
    a_ui[4] = 320.0f;  // Widget Position X
    a_ui[5] = 240.0f;  // Widget Position Y
    a_ui[6] = 800.0f;  // Widget Width
    a_ui[7] = 600.0f;  // Widget Height

    auto t0 = std::chrono::high_resolution_clock::now();
    intelligence::SubstrateState s_hat = model->predict_next_state(s_t, a_ui);
    intelligence::update_shared_state_region(s_t, a_ui, s_hat);
    auto t1 = std::chrono::high_resolution_clock::now();
    double lat_us = std::chrono::duration<double, std::micro>(t1 - t0).count();

    // What happened, and only that: a vector went through the predictor and the
    // result was published to the shared state region. Nothing reads a_t[4..7]
    // as widget bounds and nothing was drawn.
    c.o << "[JEPA LAYOUT VECTOR -> PREDICTOR -> SHARED REGION]\n";
    c.o << "a_t[0..7] fed to the predictor: [" << a_ui[0] << ", " << a_ui[1] << ", " << a_ui[2] << ", " << a_ui[3]
        << ", " << a_ui[4] << ", " << a_ui[5] << ", " << a_ui[6] << ", " << a_ui[7] << "]\n";
    c.o << "Predict + publish took: " << lat_us << " us (" << (lat_us / 1000.0) << " ms)\n";
    c.o << "Shared state region sequence: " << intelligence::get_global_shared_state_region()->sequence.load() << "\n";
    c.o << "Drawn: nothing — no compositor reads this vector as a layout; the region is data, not pixels\n";
  } else if (sub == "export") {
    // An explicit copy, to a path the caller names. Distinct from the automatic
    // checkpoint: that one is where the machine keeps its own state, this one
    // is for carrying a model somewhere else.
    if (a.size() < 3) fail(Status::Invalid, "jepa export <path>  — writes <path> and <path>.policy");
    const std::string base = c.r.expand(a[2]);
    auto mb = model->serialize();
    auto pb = policy->serialize();
    // The directory it names is made if it is missing (every parent, like mkdir -p).
    {
      const size_t slash = base.find_last_of('/');
      if (slash != std::string::npos && slash > 0) {
        const std::string dir = base.substr(0, slash);
        for (size_t at = 1; at <= dir.size(); ++at)
          if (at == dir.size() || dir[at] == '/') (void)c.k.host().make_dir(dir.substr(0, at));   // host-io: jepa export to the path the operator named
      }
    }
    need(c.k.host().write_file(base, ByteSpan(mb.data(), mb.size())));   // host-io: jepa export/import to the path the operator named
    need(c.k.host().write_file(base + ".policy", ByteSpan(pb.data(), pb.size())));   // host-io: jepa export/import to the path the operator named
    c.o << "[JEPA EXPORT] " << base << "  " << mb.size() << " B  (predictor, "
        << model->total_steps() << " step(s), weights + AdamW moments)\n";
    c.o << "[JEPA EXPORT] " << base << ".policy  " << pb.size() << " B  (policy head, "
        << policy->total_steps() << " step(s), weights + moments + return baseline)\n";
  } else if (sub == "import") {
    if (a.size() < 3) fail(Status::Invalid, "jepa import <path>  — reads <path> and <path>.policy");
    const std::string base = c.r.expand(a[2]);
    auto f1 = c.k.host().read_file(base);   // host-io: jepa export/import to the path the operator named
    if (!f1.ok()) fail(Status::Invalid, "cannot read " + base);
    std::string why;
    if (!model->deserialize(*f1, &why)) fail(Status::Invalid, base + ": " + why);
    c.o << "[JEPA IMPORT] predictor restored to " << model->total_steps()
        << " step(s), loss " << model->current_loss() << "\n";
    auto f2 = c.k.host().read_file(base + ".policy");   // host-io: jepa export/import to the path the operator named
    if (f2.ok()) {
      if (!policy->deserialize(*f2, &why)) fail(Status::Invalid, base + ".policy: " + why);
      c.o << "[JEPA IMPORT] policy head restored to " << policy->total_steps()
          << " step(s), baseline " << policy->current_baseline() << "\n";
    } else {
      c.o << "[JEPA IMPORT] no " << base << ".policy beside it — predictor only, policy head untouched\n";
    }
    // Into the machine's own checkpoint, so the next start resumes from it
    // (it used to live in this process's memory only).
    std::string note;
    if (!save_checkpoints(note)) fail(Status::IoError, "imported, but " + note);
    c.o << "[JEPA IMPORT] checkpoint written: " << note << "\n";
  } else if (sub == "reboot-resume") {
    // Called at every kernel start (cli/main.cpp), the same way the miner's is.
    // This is what makes training CUMULATIVE: without it every boot begins at
    // step 0 with freshly seeded weights, and "total steps" only ever describes
    // the session you are looking at.
    std::string note;
    const bool ok = load_checkpoints(note);
    if (!ok) c.o << "jepa checkpoint " << note << "\n";
    else if (note != "no checkpoint yet") c.o << "jepa restored — " << note << "\n";
  } else fail(Status::Invalid, "unknown: jepa status | record [count=<N>] | synthesize [count=<N>] | train [steps=<N>] | tick [count=<N>] | policy | predict | bench [steps] [batch] | latency [count=<N>] | layout | daemon | shm | export <path> | import <path> | reboot-resume   (proof of actuation: `host jepa prove /bin/spin 3`, `host jepa nic`)");
}


void cmd_qwen(Ctx& c, const Args& a) {
  std::string sub = a.size() > 1 ? a[1] : "status";
  std::string model = kv(a, "model", "spark");
  std::string url = kv(a, "url", "");
  if (url.empty()) {
    if (sub == "local" || sub == "cuda" || sub == "in-process") url = "local";
    else {
      // The host that serves the model, from the endpoints themselves.
      std::string note;
      url = resolve_model_endpoint(model, &note);
      if (url.empty()) fail(Status::NotFound, note + " — `aios models` lists what is reachable, or give url=");
    }
  }

  if (sub == "local" || sub == "cuda") {
    url = "local";
    // "qwen local status" asks about the local path, and used to be read as a
    // request to send the word "status" to it as a prompt.
    if (a.size() > 2 && a[2] == "status") sub = "status";
  }

  intelligence::QwenClient client(url, model);
  // The same credential `mind bind` sends: <root>/mind.key, else AIOS_MIND_KEY.
  // Never from the command line, never printed.
  const std::string key = intelligence::network_mind_credential(c.k.host(), c.k.config().root);
  if (!key.empty()) client.set_credential(key);
  const std::string root = c.k.config().root;

  if (sub == "status") {
    if (url == "local" || url == "cuda") {
      c.o << "Qwen in-process endpoint: the in-tree model (infer)\n";
      auto lm = userland::resident_llm(c.k.host(), root);
      if (!lm.ok()) { c.o << "Status: Unavailable — " << lm.error().message << "\n"; return; }
      c.o << "Model: " << userland::resident_llm_source() << "\n";
      c.o << "Status: Online (" << (*lm)->config().layers << " layers, context " << (*lm)->context() << ")\n";
      return;
    }
    std::string models_url = url;
    auto pos = models_url.find("/v1/");
    if (pos != std::string::npos) {
      models_url = models_url.substr(0, pos) + "/v1/models";
    }
    c.o << "Qwen LLM Endpoint: " << url << "\n";
    c.o << "Target Model: " << model << "\n";
    auto ep = channel::parse_endpoint(models_url);
    if (!ep.ok()) {
      c.o << "Status: Invalid endpoint format (" << ep.error().message << ")\n";
      return;
    }
    channel::HttpRequest req;
    req.method = "GET";
    req.endpoint = *ep;
    req.timeout_ms = 5000;
    req.host = &c.k.host();
    req.kernel_root = root;
    if (!key.empty()) req.headers.emplace_back("Authorization", "Bearer " + key);
    c.o << "Credential: " << (key.empty() ? "none (no " + root + "/mind.key, no AIOS_MIND_KEY)"
                                          : "sent (" + std::to_string(key.size()) + " chars, never printed)") << "\n";
    auto resp = channel::http_send(req);
    if (!resp.ok()) {
      // No fallback is named here any more: there was none. The line used to
      // read "Fallback: AIOS Local CUDA Mind online on arch 1080 Ti GPU" on
      // every machine, and the local path refuses every request.
      c.o << "Status: Offline/Unreachable (" << resp.error().message << ")\n";
    } else {
      c.o << "Status: Online (HTTP " << resp->status << ")\n";
      c.o << "Models Payload: " << resp->text() << "\n";
    }
  } else {
    std::string prompt;
    size_t start_idx = (sub == "ask" || sub == "prompt" || sub == "query" || sub == "local" || sub == "cuda") ? 2 : 1;
    for (size_t i = start_idx; i < a.size(); ++i) {
      if (a[i].rfind("url=", 0) == 0 || a[i].rfind("model=", 0) == 0) continue;
      prompt += (prompt.empty() ? "" : " ") + c.r.expand(a[i]);
    }
    if (prompt.empty()) prompt = "Hello";
    c.o << "Querying Qwen LLM (" << model << " @ " << url << ")... \n";
    c.o.flush();
    auto res = client.complete(prompt, &c.k.host(), root);
    if (!res.ok()) fail(res.status(), res.error().message);
    c.o << "\n[Qwen Response]\n" << *res << "\n";
  }
}

void show_object(Ctx& c, const StoredObject& o) {
  c.o << W{14} << o.id.short_str()
      << W{12} << (fmt_u64(o.timestamp_ns / 1000000000ull) + "s")
      << W{14} << o.provenance.short_str()
      << W{8}  << (fmt_u64(o.embedding.size()) + "d")
      << c.bytes_line(o.payload);
  if (!o.derived_from.is_null()) c.o << " [derived=" << o.derived_from.short_str() << "]";
  if (!o.source_state.is_null()) c.o << " [state=" << o.source_state.short_str() << "]";
  for (auto& [k, v] : o.metadata) c.o << " " << k << "=" << v;
  for (auto& r : o.relations) c.o << " rel(" << r.kind << ")→" << r.target.short_str();
  c.o << "\n";
}

void cmd_store(Ctx& c, const Args& a) {
  {
    const std::string sub = a.size() > 1 ? a[1] : "list";

  if (sub == "index" && a.size() > 2 && a[2] == "bench") {
    // Does the graph EVER win? The store's own corpus is 325 vectors and the
    // graph loses there, which justifies the crossover but says nothing about
    // whether the index is worth having at all. This builds a scratch index at
    // a given size and measures both paths on it.
    //
    // The vectors are SYNTHETIC and that is stated: the question here is how the
    // two searches scale with N, which is a property of the algorithms and not
    // of this machine's embeddings. Recall on real data is what `store index`
    // measures, above.
    const size_t n = a.size() > 3 ? (size_t)std::max(16, std::atoi(a[3].c_str())) : 4096;
    const size_t k = a.size() > 4 ? (size_t)std::max(1, std::atoi(a[4].c_str())) : 5;
    const size_t dim = 64;
    SemanticIndex idx;
    uint64_t seed = 0x2545F4914F6CDD1Dull;
    auto nextf = [&]() {
      seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
      return (float)((double)(seed >> 11) / (double)(1ull << 53)) * 2.0f - 1.0f;
    };
    std::vector<std::vector<float>> vecs;
    vecs.reserve(n);
    const uint64_t build0 = c.k.host().monotonic_ns();
    for (size_t i = 0; i < n; ++i) {
      std::vector<float> v(dim);
      for (size_t d = 0; d < dim; ++d) v[d] = nextf();
      ObjectID id{Id128{(uint64_t)(i + 1), 0x5EEDull}};
      idx.add(id, v);
      if (vecs.size() < 32) vecs.push_back(v);
    }
    // The graph is built on first use; force it here so "built in" is the
    // graph's cost, not the rows' and a first query that silently paid it.
    idx.ensure_approx();
    const uint64_t build1 = c.k.host().monotonic_ns();
    c.o << "Scratch index of " << n << " synthetic " << dim << "-d vectors\n";
    c.o << "  built in            : " << ((double)(build1 - build0) / 1e6) << " ms\n";
    c.o << "  in the graph        : " << idx.approx_size() << "\n";
    size_t found = 0, wanted = 0;
    uint64_t exact_ns = 0, approx_ns = 0;
    for (const auto& q : vecs) {
      const uint64_t t0 = c.k.host().monotonic_ns();
      const auto exact = idx.nearest_exact(q, k);
      const uint64_t t1 = c.k.host().monotonic_ns();
      const auto approx = idx.nearest_approx(q, k);
      const uint64_t t2 = c.k.host().monotonic_ns();
      exact_ns += t1 - t0;
      approx_ns += t2 - t1;
      for (const auto& e : exact) {
        ++wanted;
        for (const auto& p2 : approx) if (p2.id == e.id) { ++found; break; }
      }
    }
    const size_t trials = vecs.size();
    c.o << "Measured over " << trials << " queries, k=" << k << ":\n";
    c.o << "  recall@" << k << "          : " << (wanted ? (100.0 * (double)found / (double)wanted) : 0.0) << "%\n";
    c.o << "  exact scan          : " << (trials ? (double)exact_ns / (double)trials / 1000.0 : 0.0) << " us/query\n";
    c.o << "  graph               : " << (trials ? (double)approx_ns / (double)trials / 1000.0 : 0.0) << " us/query\n";
    if (approx_ns) {
      const double ratio = (double)exact_ns / (double)approx_ns;
      c.o << "  the graph is " << ratio << "x the scan's speed"
          << (ratio > 1.05 ? " - FASTER" : ratio < 0.95 ? " - slower" : " - a wash") << "\n";
    }
    return;
  }

  if (sub == "index") {
    // What the similarity search actually is on this machine, and what the
    // approximate path COSTS in answers — measured against the exact scan, not
    // asserted. An ANN index is a trade and a trade you have not measured is a
    // claim.
    const auto& store = c.k.store();
    const size_t n = store.semantic_size();
    c.o << "Similarity index\n";
    c.o << "  vectors held        : " << n << "\n";
    c.o << "  in the graph        : " << store.semantic_approx_size()
        << (store.semantic_approx_ready() ? "" : n ? "  (not built yet: built by the first query that uses it)"
                                                    : "  (no graph built: nothing has been added yet)") << "\n";
    c.o << "  path a query takes  : "
        << (n >= SemanticIndex::kApproxFrom
                ? "the navigable small-world graph (sublinear)"
                : "the exact linear scan")
        << "\n";
    c.o << "  crossover           : " << SemanticIndex::kApproxFrom
        << " vectors, measured (`store index bench`): the graph is 4.4x the scan's\n"
        << "                        speed at 1024, 9.7x at 8192 and 27x faster at 32768,\n"
        << "                        at 98-100% recall@5. Below the crossover the scan is\n"
        << "                        faster AND exact, so using the graph there is unnecessary.\n";
    if (n < 8) {
      c.o << "too few vectors to measure recall against; add objects with embeddings first\n";
      return;
    }
    // Recall@k: for a sample of the vectors already held, ask both paths and
    // count how many of the exact answer's ids the graph also returned. Every
    // query is a vector the store really has, so this is recall on this
    // machine's own distribution rather than on random noise.
    const size_t k = a.size() > 2 ? (size_t)std::max(1, std::atoi(a[2].c_str())) : 5;
    const size_t trials = std::min<size_t>(24, n);
    size_t found = 0, wanted = 0;
    uint64_t exact_ns = 0, approx_ns = 0;
    // The graph is built by the first query that uses it. Pay that outside the
    // timer, or the first trial carries the whole build and the per-query
    // figure is a build time divided by 24.
    (void)store.semantic_approx(store.semantic_vector_at(0), k);
    for (size_t t = 0; t < trials; ++t) {
      const auto q = store.semantic_vector_at((t * 7919) % n);
      if (q.empty()) continue;
      const uint64_t t0 = c.k.host().monotonic_ns();
      const auto exact = store.semantic_exact(q, k);
      const uint64_t t1 = c.k.host().monotonic_ns();
      const auto approx = store.semantic_approx(q, k);
      const uint64_t t2 = c.k.host().monotonic_ns();
      exact_ns += t1 - t0;
      approx_ns += t2 - t1;
      // A hit is right when it is as near as the exact k-th answer, not when
      // it is the same id: this store holds 729 distinct vectors among 1,649,
      // and ~23 of them tie at the 5th-best score, so matching ids scored which
      // of the equally-near duplicates a search happened to pick (30%).
      if (exact.empty()) continue;
      const float kth = exact.back().score;
      wanted += exact.size();
      for (size_t i = 0; i < approx.size() && i < exact.size(); ++i)
        if (approx[i].score >= kth - 1e-6f) ++found;
    }
    c.o << "Measured over " << trials << " queries drawn from the store's own vectors, k=" << k << ":\n";
    c.o << "  recall@" << k << "          : " << (wanted ? (100.0 * (double)found / (double)wanted) : 0.0)
        << "%  (" << found << " of " << wanted << " hits as near as the exact k-th answer)\n";
    c.o << "  exact scan          : " << (trials ? (double)exact_ns / (double)trials / 1000.0 : 0.0) << " us/query\n";
    c.o << "  graph               : " << (trials ? (double)approx_ns / (double)trials / 1000.0 : 0.0) << " us/query\n";
    if (approx_ns > 0 && exact_ns > 0) {
      const double ratio = (double)exact_ns / (double)approx_ns;
      c.o << "  the graph is " << ratio << "x the scan's speed"
          << (ratio > 1.05 ? " - faster" : ratio < 0.95 ? " - SLOWER, which at this size is expected" : " - a wash")
          << "\n";
    }
    return;
  }

  }

  if (a.size() < 2) fail(Status::Invalid, "store query|put|get [<e>] <obj> [show=]|similar|related|history|snapshot|list|meta");
  const std::string& sub = a[1];
  if (sub == "query") {
    if (a.size() < 3) fail(Status::Invalid, "store query [<e>] <aql_statement>");
    const bool named = a.size() >= 4 && (a[2].rfind("SELECT", 0) != 0 && a[2].rfind("PUT", 0) != 0 && a[2].rfind("INSERT", 0) != 0);
    Surface who = named ? c.as(a[2]) : c.me;
    // The statement is the rest of the line AS TYPED, quotes included: AQL has
    // its own string literals, and rejoining the console's tokens (which have
    // had their quotes stripped) turned PUT "via aql" into a payload of `aql`.
    size_t qstart = named ? 3 : 2;
    std::string qstr;
    {
      std::string L = g_raw_line;
      auto arrow = L.rfind("=>");          // the console's own `=> NAME` binding is not AQL
      if (arrow != std::string::npos) L = L.substr(0, arrow);
      size_t p = L.find_first_not_of(" \t");
      if (p != std::string::npos && L[p] == '!') ++p;
      bool matches = true;
      for (size_t w = 0; w < qstart && matches; ++w) {   // skip `store query [<e>]`, quote-aware
        p = L.find_first_not_of(" \t", p);
        if (p == std::string::npos) { matches = false; break; }
        std::string word;
        char q = 0;
        while (p < L.size() && (q || (L[p] != ' ' && L[p] != '\t'))) {
          if (q) { if (L[p] == q) q = 0; else word += L[p]; }
          else if (L[p] == '"' || L[p] == '\'') q = L[p];
          else word += L[p];
          ++p;
        }
        if (word != a[w]) matches = false;
      }
      if (matches && p != std::string::npos) {
        size_t b = L.find_first_not_of(" \t", p);
        if (b != std::string::npos) qstr = L.substr(b);
        while (!qstr.empty() && (qstr.back() == ' ' || qstr.back() == '\t' || qstr.back() == '\r' || qstr.back() == '\n')) qstr.pop_back();
      }
      if (qstr.empty())   // not typed as a line (e.g. run from elsewhere): the tokens are all there is
        for (size_t i = qstart; i < a.size(); ++i) { if (i > qstart) qstr += " "; qstr += a[i]; }
    }
    // $name outside quotes is a console variable, so an id bound with `=> x`
    // can be used in RELATED TO $x / HISTORY OF $x.
    {
      std::string out;
      char q = 0;
      for (size_t i = 0; i < qstr.size(); ++i) {
        char ch = qstr[i];
        if (q) { if (ch == '\\' && i + 1 < qstr.size()) { out += ch; out += qstr[++i]; continue; } if (ch == q) q = 0; out += ch; continue; }
        if (ch == '"' || ch == '\'') { q = ch; out += ch; continue; }
        if (ch == '$' && i + 1 < qstr.size() && (std::isalnum((unsigned char)qstr[i + 1]) || qstr[i + 1] == '_')) {
          size_t j = i + 1;
          while (j < qstr.size() && (std::isalnum((unsigned char)qstr[j]) || qstr[j] == '_')) ++j;
          out += c.r.expand(qstr.substr(i, j - i));
          i = j - 1;
          continue;
        }
        out += ch;
      }
      qstr = out;
    }
    auto res = aios::storage::execute_aql(who, qstr);
    if (!res.success) fail(Status::Invalid, "AQL error: " + res.error);
    if (!res.objects.empty()) c.produced(res.objects.front().id.str());   // `=> x` binds the first row
    c.o << res.formatted;
    return;
  }
  if (sub == "put") {
    if (a.size() < 4) fail(Status::Invalid, "store put <e> <text> [derived=<obj>] [rel=<obj>:<kind>] [k=v]...");
    StoredObject o;
    o.payload = parse_bytes(a[3]);
    o.embedding = embed_text(a[3]);
    for (size_t i = 4; i < a.size(); ++i) {
      auto eq = a[i].find('=');
      if (eq == std::string::npos) fail(Status::Invalid, "expected k=v, got '" + a[i] + "'");
      std::string k = a[i].substr(0, eq), v = a[i].substr(eq + 1);
      if (k == "derived") o.derived_from = c.r.as<ObjectID>(v);
      else if (k == "rel") { auto colon = v.find(':'); if (colon == std::string::npos) fail(Status::Invalid, "rel=<obj>:<kind>"); o.relations.push_back({c.r.as<ObjectID>(v.substr(0, colon)), (uint32_t)to_u64(v.substr(colon + 1))}); }
      else o.metadata.emplace_back(k, v);
    }
    auto id = need(c.as(a[2]).store(std::move(o)));
    c.produced(id.str());
    c.o << "object " << id.str() << "\n";
  } else if (sub == "get") {
    if (a.size() < 3) fail(Status::Invalid, "store get [<e>] <obj> [show=<chars>] — the object and what it says");
    // `store get <obj>` reads as yourself; `store get <e> <obj>` reads as <e>.
    // A mind that wrote something has to be able to read it back in full — the
    // one-line summary alone cost twelve cycles of `store get … show=2000`.
    const bool named = a.size() >= 4 && a[3].find('=') == std::string::npos;
    const size_t oi = named ? 3 : 2;
    Surface who = named ? c.as(a[2]) : c.me;
    size_t show = has_kv(a, "show") ? (size_t)to_u64(kv(a, "show")) : 2400;
    auto o = need(who.fetch(c.r.as<ObjectID>(a[oi])));
    show_object(c, o);
    std::string text(o.payload.begin(), o.payload.end());
    bool printable = true;
    for (unsigned char ch : text) if (ch < 9 || (ch > 13 && ch < 32)) { printable = false; break; }
    if (!printable) {
      c.o << "  (" << fmt_bytes(o.payload.size()) << " of binary payload)\n";
    } else {
      std::string t = text.size() > show ? text.substr(0, show) : text;
      size_t at = 0;
      while (at < t.size()) {
        size_t nl = t.find('\n', at);
        std::string line = t.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        if (!line.empty()) c.o << "  | " << line << "\n";
        if (nl == std::string::npos) break;
        at = nl + 1;
      }
      if (text.size() > show) c.o << "  | …(" << (text.size() - show) << " more chars: show=" << text.size() << ")\n";
    }
    c.produced(o.id.str());
  } else if (sub == "similar") {
    if (a.size() < 3) fail(Status::Invalid, "store similar <text…> [k]");
    // The text may be several unquoted words; only a TRAILING bare integer is k.
    // A mind typing `store similar chromium sandbox rce` should get neighbours,
    // not "expected a number, got 'sandbox'" and a wasted cycle.
    size_t k = 5, last = a.size();
    { uint64_t v; if (a.size() > 3 && parse_u64(a.back(), v) && v > 0) { k = (size_t)v; last = a.size() - 1; } }
    std::string text; for (size_t i = 2; i < last; ++i) { if (!text.empty()) text += " "; text += a[i]; }
    c.view(); auto hits = c.k.store().find_similar(embed_text(text), k);
    for (auto& h : hits) { auto o = c.k.store().get(h.id); c.o << Fixed{3} << h.score << "  "; if (o.ok()) show_object(c, *o); }
    if (!hits.empty()) c.produced(hits[0].id.str());
  } else if (sub == "related") {
    if (a.size() < 3) fail(Status::Invalid, "store related <obj>");
    c.view(); for (auto& r : c.k.store().find_related(c.r.as<ObjectID>(a[2]))) { auto o = c.k.store().get(r.target); c.o << "kind " << r.kind << "  "; if (o.ok()) show_object(c, *o); }
  } else if (sub == "history") {
    if (a.size() < 3) fail(Status::Invalid, "store history <obj>");
    c.view(); for (auto& id : c.k.store().find_history(c.r.as<ObjectID>(a[2]))) { auto o = c.k.store().get(id); if (o.ok()) show_object(c, *o); }
  } else if (sub == "snapshot") {
    if (a.size() < 4) fail(Status::Invalid, "store snapshot <e> <state>");
    StateID st = c.r.as<StateID>(a[3]);
    auto so = c.k.states().copy(st);
    std::vector<float> emb; if (so.ok()) emb = embed_text(std::string(so->payload.begin(), so->payload.end()));
    auto id = need(c.as(a[2]).snapshot(st, emb));
    c.produced(id.str());
    c.o << "object " << id.str() << " (snapshot of state " << st.short_str() << ")\n";
  } else if (sub == "meta") {
    if (a.size() < 3) fail(Status::Invalid, "store meta <key> [value]");
    c.view(); for (auto& id : c.k.store().find_meta(a[2], a.size() > 3 ? a[3] : "")) { auto o = c.k.store().get(id); if (o.ok()) show_object(c, *o); }
  } else if (sub == "list") {
    c.view();
    c.o << c.o.style(Header())
        << W{14} << "OBJECT ID"
        << W{12} << "TIMESTAMP"
        << W{14} << "PROVENANCE"
        << W{8}  << "EMBED"
        << "PAYLOAD / DETAILS" << c.o.style(Reset()) << "\n";
    for (auto& id : c.k.store().list()) { auto o = c.k.store().get(id); if (o.ok()) show_object(c, *o); }
  } else if (sub == "status" || sub == "blocks") {
    c.view();
    const auto& st = c.k.store();
    c.o << c.o.style(Header()) << "ObjectStore Tiering & Sparse Block Index Status" << c.o.style(Reset()) << "\n";
    c.o << "  total objects       : " << st.count() << "\n";
    c.o << "  active (dense map)  : " << st.active_count() << "\n";
    c.o << "  cold (sparse blocks): " << st.cold_objects_count() << " objects across " << st.cold_blocks_count() << " sealed block(s)\n";
    c.o << "  sparse index memory : " << (st.cold_blocks_count() * sizeof(ObjectStore::SparseBlock)) << " bytes in RAM\n";
    c.o << "  decoded hot cache   : " << (st.hot_bytes() / 1024) << " KiB / " << (st.hot_budget() / (1024 * 1024)) << " MiB (" << st.hot_count() << " objects resident)\n";
  } else if (sub == "seal") {
    c.k.store().seal_cold_blocks();
    c.o << "sealed active objects into cold sparse blocks; cold objects now: " << c.k.store().cold_objects_count() << "\n";
  } else fail(Status::Invalid, "unknown: store " + sub);
}

void cmd_node(Ctx& c, const Args& a) {
  if (a.size() < 2) fail(Status::Invalid, "node serve <port> <token> | connect <host> <port> <token> | memory [<#>] | peers | list");
  const std::string& sub = a[1];
  if (sub == "serve") {
    if (a.size() < 4) fail(Status::Invalid, "node serve <port> <token|@FILE|env:NAME> [bind=127.0.0.1|0.0.0.0]");
    const bool loopback = bind_loopback_arg(a);
    auto p = need(c.me.serve((uint16_t)to_u64(a[2]), fleet_key_arg(c.r.expand(a[3])), c.s.polled_services, loopback));
    c.s.vars["port"] = std::to_string(p);
    c.o << "serving this kernel on " << (loopback ? "127.0.0.1:" : "0.0.0.0:") << p << " (peers act here as guest entities; link authenticated and encrypted with the token)\n";
  } else if (sub == "connect") {
    if (a.size() < 5) fail(Status::Invalid, "node connect <host> <port> <token>");
    auto at = need(c.me.connect(c.r.expand(a[2]), (uint16_t)to_u64(c.r.expand(a[3])), c.r.expand(a[4])));
    c.o << "attached node " << at.address << ": " << at.devices.size() << " device(s)";
    for (auto& d : at.devices) c.o << " [" << d << "]";
    c.o << (at.provides_memory ? ", provides remote memory" : "") << ", transport " << at.transport << "\n";
    c.s.vars["transport"] = at.transport;
  } else if (sub == "memory") {
    // node memory [<node-index>]   which attached peer new remote memory goes to
    if (a.size() > 2) {
      const std::string addr = need(choose_remote_memory(c.k, (size_t)to_u64(c.r.expand(a[2]))));
      c.o << "new remote memory now goes to node #" << c.r.expand(a[2]) << " (" << addr << "); regions already placed stay where they are\n";
    } else {
      const std::string cur = remote_memory_node(c.k);
      if (cur.empty()) c.o << "no attached peer provides remote memory (node connect, or fleet join and reach a peer)\n";
      else c.o << "new remote memory goes to " << cur << "\n";
      for (size_t i = 0; i < c.k.nodes().size(); ++i)
        c.o << "  #" << i << "  " << c.k.nodes()[i]->address() << (c.k.nodes()[i]->address() == cur ? "  (chosen)" : "") << "\n";
      c.o << "  choose another: node memory <#>\n";
    }
  } else if (sub == "migrate") {
    if (a.size() < 3) fail(Status::Invalid, "node migrate <e> [node-index=0] [keep]   move a running entity to a connected peer kernel");
    EntityID e = c.r.as<EntityID>(a[2]);
    size_t idx = a.size() > 3 && a[3] != "keep" ? (size_t)to_u64(a[3]) : 0;
    bool keep = false; for (size_t i = 3; i < a.size(); ++i) if (a[i] == "keep") keep = true;
    auto remote = need(c.me.migrate(e, idx, keep));
    c.produced(remote.str());
    c.o << (keep ? "cloned " : "migrated ") << e.short_str() << " to peer node #" << idx << " — it lives there as " << remote.str() << (keep ? "" : " and is gone here") << "\n";
  } else if (sub == "peers") {
    c.view();
    if (!c.k.service()) fail(Status::NotFound, "not serving");
    for (auto& p : c.k.service()->peers()) { c.o << "guest " << p.guest.str() << " from " << p.address << " kernel " << p.kernel.short_str() << " requests=" << p.requests << (p.connected ? "" : " (gone)") << "\n"; c.produced(p.guest.str()); }
  } else if (sub == "list") {
    c.view();
    for (auto& n : c.k.nodes()) { c.o << n->address() << " kernel " << n->kernel().short_str() << " calls=" << n->calls() << " devices=" << n->devices().size() << "\n"; }
  } else fail(Status::Invalid, "unknown: node " + sub);
}

void cmd_p2p(Ctx& c, const Args& a) {
  // The node id comes from this machine's kernel identity: the default
  // constructor seeds every machine with one constant, so all announced the
  // same id and each dropped the others' beacons as its own.
  static aios::channel::P2PMeshDiscovery mesh(c.k.self().str());
  if (a.size() < 2) {
    c.o << mesh.render_status();
    return;
  }
  const std::string& sub = a[1];
  if (sub == "status") {
    c.o << mesh.render_status();
  } else if (sub == "peers") {
    // Listen on the LAN for the beacons peers send (every ~5 s), then list who was heard.
    const int wait_ms = (int)to_u64(kv(a, "wait", "6000"));
    auto heard = mesh.poll_network(wait_ms);
    if (!heard.ok()) fail(heard.error().status, "listening on udp/" + std::to_string(mesh.kBeaconPort) + ": " + heard.error().str());
    auto list = mesh.discover_peers();
    const uint64_t now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    c.o << c.o.style(Header()) << "P2P peers heard on udp/" << mesh.kBeaconPort << " in " << wait_ms << " ms (" << list.size()
        << ")" << c.o.style(Reset()) << "\n";
    if (list.empty()) c.o << "  (no beacon heard - peers announce every ~5 s; try wait=12000)\n";
    for (const auto& peer : list) {
      c.o << "  " << peer.node_id << " @ " << peer.ip_address << ":" << peer.port << "  heard "
          << (now_ms > peer.last_seen_ms ? now_ms - peer.last_seen_ms : 0) << " ms ago"
          << (peer.is_active ? "" : " (announced inactive)") << "\n";
      c.produced(peer.node_id);
    }
  } else if (sub == "ping") {
    std::string nid = (a.size() > 2 && a[2].find('=') == std::string::npos) ? a[2] : kv(a, "node_id", "");
    std::string ip = (a.size() > 3 && a[3].find('=') == std::string::npos) ? a[3] : kv(a, "ip", "");
    uint16_t port = static_cast<uint16_t>(to_u64(kv(a, "port", "7421")));
    if (a.size() > 4 && a[4].find('=') == std::string::npos) port = static_cast<uint16_t>(to_u64(a[4]));
    if (nid.empty() || ip.empty()) fail(Status::Invalid, "p2p ping <node_id> <ip> [port]");
    auto res = mesh.ping_peer(nid, ip, port);
    if (!res.ok()) fail(res.error().status, "[P2P UNREACHABLE] " + res.error().str());
    c.o << c.o.style(Green()) << "[P2P REACHABLE] " << c.o.style(Reset()) << nid << " (" << ip << ":" << port
        << ") accepted a TCP connection; added to the routing table\n";
  } else if (sub == "store") {
    std::string k = kv(a, "key", "");
    std::string v = kv(a, "value", "");
    if (k.empty() && a.size() > 2) k = a[2];
    if (v.empty() && a.size() > 3) v = a[3];
    if (k.empty() || v.empty()) fail(Status::Invalid, "p2p store key=<k> value=<v>");

    auto res = mesh.rpc_store(k, v, mesh.local_node_id());
    if (!res.ok()) {
      c.o << c.o.style(Red()) << "[P2P STORE ERROR] " << res.error().str() << c.o.style(Reset()) << "\n";
    } else {
      c.o << c.o.style(Green()) << "[P2P STORE SUCCESS] " << c.o.style(Reset())
          << "Stored key='" << k << "' value='" << v << "' in local DHT partition\n";
    }
  } else if (sub == "find-value") {
    std::string k = (a.size() > 2 && a[2].find('=') == std::string::npos) ? a[2] : kv(a, "key", "");
    if (k.empty()) fail(Status::Invalid, "p2p find-value <key>");

    auto res = mesh.rpc_find_value(k);
    if (res.found) {
      c.o << c.o.style(Green()) << "[DHT VALUE FOUND] " << c.o.style(Reset()) << "\n"
          << "  Key       : " << res.value.key << "\n"
          << "  Value     : " << res.value.value << "\n"
          << "  Publisher : " << res.value.publisher_id << "\n";
    } else {
      c.o << c.o.style(Yellow()) << "[DHT VALUE NOT FOUND IN LOCAL PARTITION]" << c.o.style(Reset()) << "\n"
          << "  Closest peers for target key:\n";
      for (const auto& peer : res.closest_peers) {
        c.o << "    Node ID: " << peer.node_id << " @ " << peer.ip_address << ":" << peer.port << "\n";
      }
    }
  } else if (sub == "find-node") {
    std::string target = (a.size() > 2 && a[2].find('=') == std::string::npos) ? a[2] : kv(a, "target", "");
    if (target.empty()) fail(Status::Invalid, "p2p find-node <target_id>");

    auto peers = mesh.find_closest_peers(target, 8);
    c.o << c.o.style(Header()) << "Kademlia Closest Peers to Target ID: " << target << c.o.style(Reset()) << "\n";
    for (const auto& peer : peers) {
      c.o << "  Node ID: " << peer.node_id << " @ " << peer.ip_address << ":" << peer.port << "\n";
    }
  } else if (sub == "mdns") {
    std::string action = (a.size() > 2) ? a[2] : "announce";
    uint16_t port = static_cast<uint16_t>(to_u64(kv(a, "port", "5353")));
    if (action == "announce" || action == "start") {
      // announce: one beacon now. every=S [count=N]: keep announcing (count=0: until stopped).
      const uint64_t every = to_u64(kv(a, "every", "0"));
      const uint64_t count = to_u64(kv(a, "count", every ? "0" : "1"));
      for (uint64_t n = 0; count == 0 || n < count; ++n) {
        auto sent = mesh.start_mdns_beacon(port);
        if (!sent.ok()) fail(sent.error().status, "[mDNS BEACON NOT SENT] " + sent.error().str());
        c.o << c.o.style(Green()) << "[mDNS BEACON SENT] " << c.o.style(Reset()) << mesh.generate_mdns_announcement(port)
            << " -> 255.255.255.255:" << mesh.kBeaconPort << "\n";
        c.o.flush();
        if (count != 0 && n + 1 >= count) break;
        aios::hal::sleep_ms((uint32_t)(every ? every : 5) * 1000u);
      }
    } else if (action == "parse") {
      std::string payload = (a.size() > 3) ? a[3] : kv(a, "payload", "");
      auto res = mesh.parse_mdns_announcement(payload);
      if (!res.ok()) {
        c.o << c.o.style(Red()) << "[mDNS PARSE ERROR] " << res.error().str() << c.o.style(Reset()) << "\n";
      } else {
        c.o << c.o.style(Green()) << "[mDNS PEER DISCOVERED] " << c.o.style(Reset())
            << "Added peer " << res.value().node_id << " via subnet beacon\n";
      }
    } else if (action == "stop") {
      mesh.stop_mdns_beacon();
      c.o << "[mDNS BEACON STOPPED]\n";
    } else {
      fail(Status::Invalid, "p2p mdns announce|parse|stop");
    }
  } else {
    fail(Status::Invalid, "p2p status|peers|ping|store|find-value|find-node|mdns");
  }
}

// One placement brain per kernel, kept for the life of the process so its
// decision log accumulates across commands. The orchestrator is a kernel
// subsystem (like the compute scheduler); the command here only drives and
// inspects it — it never reaches kernel authority itself.
static Orchestrator& orch_for(Kernel& k) {
  static std::unordered_map<Kernel*, Orchestrator> m;
  auto it = m.find(&k);
  if (it == m.end())
    it = m.emplace(std::piecewise_construct, std::forward_as_tuple(&k), std::forward_as_tuple(k)).first;
  return it->second;
}
static std::string pf(double d) { return fmt_double(d, 6, true, 2); }

static void fleet_show_telemetry(Ctx& c, Orchestrator& orch) {
  NodeTelemetry L = orch.local();
  c.o << c.o.style(Header()) << "Fleet load" << c.o.style(Reset()) << "  (high-water " << pf(orch.high()) << ", low-water " << pf(orch.low()) << ")\n";
  c.o << "  " << c.o.style(Cyan()) << "* this  " << c.o.style(Reset()) << L.kernel.short_str()
      << "  jobs " << L.movable_entities << "/" << L.slots << " slots"
      << "  pressure " << pf(L.pressure()) << "  free " << L.free_slots()
      << "  (" << L.logical_cpus << " cpus)\n";
  auto kernels_line = [&](const NodeTelemetry& t) {
    if (t.custom_kernels.empty()) return;
    c.o << "        kernels:";
    for (size_t i = 0; i < t.custom_kernels.size(); ++i) c.o << (i ? ", " : " ") << t.custom_kernels[i];
    c.o << "\n";
  };
  kernels_line(L);
  auto peers = orch.survey();
  if (peers.empty()) {
    if (c.k.in_fleet()) {
      auto roster = c.k.fleet_roster();
      if (roster.empty()) c.o << "  (in a fleet; no other members attached yet)\n";
      else c.o << "  (in a fleet; " << roster.size() << " peer(s) in the roster, reached on the next tick)\n";
    } else {
      c.o << "  (no peers — `fleet join <hub> <port> <key>`, or `node connect` for a direct link)\n";
    }
  }
  for (auto& v : peers) {
    c.o << "  " << v.index << "  " << v.address << "  ";
    if (!v.reachable) { c.o << c.o.style(Red()) << "unreachable" << c.o.style(Reset()) << " (kernel " << v.tele.kernel.short_str() << ")\n"; continue; }
    c.o << v.tele.kernel.short_str() << "  jobs " << v.tele.movable_entities << "/" << v.tele.slots << " slots"
        << "  pressure " << pf(v.tele.pressure()) << "  free " << v.tele.free_slots() << "\n";
    kernels_line(v.tele);
  }
}

static void fleet_show_decisions(Ctx& c, const std::vector<PlacementDecision>& ds, bool executed) {
  for (auto& p : ds) {
    const char* mark = p.kind == PlacementDecision::Hold ? "·" : (executed && p.executed ? "✓" : (executed ? "✗" : "→"));
    c.o << "  " << mark << " " << W{10} << p.kind_name();
    if (!p.entity.is_null()) c.o << " job " << p.entity.short_str();
    if (p.kind != PlacementDecision::Hold) c.o << "  " << p.from.short_str() << " → " << p.to.short_str();
    c.o << "  — " << p.reason << "\n";
    if (p.kind == PlacementDecision::Rebalance && !p.entity.is_null() && executed && p.executed) c.produced(p.to.str());
  }
}

void cmd_fleet(Ctx& c, const Args& a) {
  Orchestrator& orch = orch_for(c.k);
  const std::string sub = a.size() > 1 ? a[1] : "status";
  if (sub == "status" || sub == "telemetry" || sub == "load") {
    c.view();
    fleet_show_telemetry(c, orch);
  } else if (sub == "hub") {
    if (a.size() < 4) fail(Status::Invalid, "fleet hub <port> <cluster-key|@FILE|env:NAME> [bind=127.0.0.1|0.0.0.0] [members=<file>] [attest=require]   become the fleet's rendezvous + relay");
    const bool loopback = bind_loopback_arg(a);
    const std::string members = has_kv(a, "members") ? c.r.expand(kv(a, "members")) : std::string();
    const bool require_attest = has_kv(a, "attest") && kv(a, "attest") == "require";
    auto p = need(c.me.relay_hub((uint16_t)to_u64(a[2]), fleet_key_arg(c.r.expand(a[3])), loopback, members, require_attest));
    c.s.vars["hub_port"] = std::to_string(p);
    c.o << "fleet hub up on " << (loopback ? "127.0.0.1:" : "0.0.0.0:") << p << " — members attach here; it keeps the roster and relays circuits, seeing only ciphertext\n";
  } else if (sub == "identity") {
    // This machine's own key (stage 1 of docs/design/fleet-identity-keys.md): the public half, for the hub's roster.
    // The private half stays in <root>/node.identity.
    const crypto::Point32* pub = c.k.node_public_key();
    if (!pub) fail(Status::NotFound, "this machine has no node identity: " + c.k.node_key_problem());
    c.o << "kernel=" << c.k.self().str() << " ed25519=" << hex(ByteSpan(pub->data(), pub->size())) << "\n";
  } else if (sub == "join") {
    if (a.size() < 5) fail(Status::Invalid, "fleet join <hub-host> <hub-port> <cluster-key> [retry] [member-key=<key|@FILE|env:NAME>] [attest=1] [hub-key=<hex|@FILE>]   join a fleet through a hub; retry: keep trying while the hub is down; member-key: this member's own attach key, for a hub with a members file; attest=1: present a TPM quote; hub-key: the hub's `fleet identity` key - peers' keys are then taken from the rosters it signs");
    if (has_kv(a, "hub-key")) {
      std::string h = fleet_key_arg(c.r.expand(kv(a, "hub-key")));
      while (!h.empty() && (h.back() == '\n' || h.back() == '\r' || h.back() == ' ')) h.pop_back();
      const ByteBuffer kb = crypto::hex_to_bytes(h);
      if (h.size() != 64 || kb.size() != 32) fail(Status::Invalid, "hub-key must be 64 hex digits (the hub's `fleet identity` ed25519=)");
      crypto::Point32 hk{}; std::memcpy(hk.data(), kb.data(), 32);
      c.k.set_fleet_hub_key(hk);
    }
    bool retry = false;
    for (size_t i = 5; i < a.size(); ++i) if (a[i] == "retry") retry = true;
    const std::string member_key = has_kv(a, "member-key") ? fleet_key_arg(c.r.expand(kv(a, "member-key"))) : std::string();
    const bool attest = has_kv(a, "attest") && kv(a, "attest") == "1";
    need(c.me.fleet_join(c.r.expand(a[2]), (uint16_t)to_u64(c.r.expand(a[3])), fleet_key_arg(c.r.expand(a[4])), retry, member_key, attest));
    // A member answers peers' SQL from its own engine (sql_on from anywhere
    // in the fleet); an observer, below, only surveys.
    if (c.k.service()) userland::serve_machine_sql(*c.k.service(), c.k);
    if (!c.k.in_fleet())
      c.o << "the hub at " << c.r.expand(a[2]) << ":" << c.r.expand(a[3]) << " is not reachable ("
          << (c.k.relay() ? c.k.relay()->last_error() : std::string("unknown")) << "); this machine keeps trying in the background — `fleet members` shows when it attaches\n";
    else
      c.o << "joined the fleet via " << c.r.expand(a[2]) << ":" << c.r.expand(a[3]) << " — roster: " << c.k.fleet_roster().size() << " peer(s) discovered\n";
  } else if (sub == "run") {
    // fleet run <peer> <command...>   or   fleet run <peer> @FILE
    // One of the peer's own commands, carried by the node link (end to end
    // encrypted through the hub): what it printed, and fail on a nonzero code.
    // Only a peer that installed a command runner answers (the Pi does).
    if (a.size() < 4) fail(Status::Invalid, "fleet run <peer-id-prefix> <command...|@FILE>   run a command on a fleet peer");
    if (!c.k.in_fleet()) fail(Status::Busy, "not in a fleet - `fleet join <hub> <port> <key|@FILE>` first");
    const std::string want = c.r.expand(a[2]);
    std::vector<Id128> hits;
    for (auto& id : c.k.fleet_roster()) if (id.str().compare(0, want.size(), want) == 0) hits.push_back(id);
    if (hits.size() != 1) {
      std::string known;
      for (auto& id : c.k.fleet_roster()) known += " " + id.short_str();
      fail(Status::NotFound, (hits.empty() ? "no fleet member starts with " : "more than one fleet member starts with ") + want + " (roster:" + known + ")");
    }
    std::string line;
    if (a.size() == 4 && !a[3].empty() && a[3][0] == '@') {
      std::vector<std::string> lines;
      if (!read_file_lines(a[3].substr(1), lines)) fail(Status::NotFound, "cannot read " + a[3].substr(1));
      for (size_t i = 0; i < lines.size(); ++i) line += (i ? "\n" : "") + lines[i];
    } else {
      for (size_t i = 3; i < a.size(); ++i) line += (i > 3 ? " " : "") + a[i];
    }
    size_t at = need(c.k.reach(hits[0]));
    auto res = need(c.k.nodes()[at]->command(line));
    c.o << res.second;
    if (!res.second.empty() && res.second.back() != '\n') c.o << "\n";
    if (res.first != 0) fail(Status::IoError, "exit code " + std::to_string(res.first) + " on " + hits[0].short_str());
  } else if (sub == "members") {
    c.view();
    if (c.k.fleet_reconnecting()) {
      const std::string why = c.k.relay()->last_error();
      c.o << "  (in a fleet, but not attached to the hub at " << c.k.relay()->hub_address() << " right now — reconnecting"
          << (why.empty() ? std::string("") : ": " + why) << ")\n";
    }
    else if (!c.k.in_fleet()) { c.o << "  (not in a fleet — `fleet join <hub> <port> <key>` or `fleet hub <port> <key>`)\n"; }
    else {
      auto roster = c.k.fleet_roster();
      c.o << c.o.style(Header()) << "Fleet roster" << c.o.style(Reset()) << " (discovered via the hub, self excluded)\n";
      if (roster.empty()) c.o << "  (no other members attached yet)\n";
      for (auto& id : roster) c.o << "  " << id.short_str() << "\n";
    }
  } else if (sub == "observe") {
    // fleet observe <hub-host> <hub-port> <key> [status-file] [interval-ms]
    // Join the hub as a read-only observer and continuously write the live
    // fleet — every member's identity and load — to a JSON file the console
    // (or anything else) can serve. Runs until stopped.
    if (a.size() < 5) fail(Status::Invalid, "fleet observe <hub-host> <hub-port> <key> [status-file=/opt/aos/fleet-status.json] [interval-ms=4000] [member-key=<key|@FILE|env:NAME>]");
    // member-key=: the observer's own attach key, for a hub with a members file
    const std::string obs_member_key = has_kv(a, "member-key") ? fleet_key_arg(c.r.expand(kv(a, "member-key"))) : std::string();
    need(c.me.fleet_join(c.r.expand(a[2]), (uint16_t)to_u64(c.r.expand(a[3])), fleet_key_arg(c.r.expand(a[4])), false, obs_member_key));
    std::vector<std::string> pos;   // status-file, interval-ms: the words after the key that are no name=value
    for (size_t i = 5; i < a.size(); ++i) if (a[i].rfind("member-key=", 0) != 0) pos.push_back(a[i]);
    std::string path = pos.size() > 0 ? c.r.expand(pos[0]) : std::string("/opt/aos/fleet-status.json");
    uint32_t interval = pos.size() > 1 ? (uint32_t)to_u64(pos[1]) : 4000;
    std::string hub = c.r.expand(a[2]) + ":" + c.r.expand(a[3]);
    c.o << "observing the fleet via " << hub << "; writing " << path << " every " << interval << "ms (stop to end)\n";
    auto dbj = [](const std::vector<NodeTelemetry::HostedDbInfo>& dbs) {
      std::string s = "[";
      for (size_t i = 0; i < dbs.size(); ++i) {
        if (i) s += ",";
        s += "{\"db_name\":\"" + dbs[i].db_name + "\""
           + ",\"asql_port\":" + std::to_string(dbs[i].asql_port)
           + ",\"mysql_port\":" + std::to_string(dbs[i].mysql_port)
           + ",\"table_count\":" + std::to_string(dbs[i].table_count)
           + ",\"read_only\":" + (dbs[i].read_only ? "true" : "false") + "}";
      }
      s += "]";
      return s;
    };
    auto tj = [&dbj](const NodeTelemetry& t, const std::string& id) {
      return "{\"id\":\"" + id + "\""
           + ",\"cpus\":" + std::to_string(t.logical_cpus)
           + ",\"slots\":" + std::to_string(t.slots)
           + ",\"jobs\":" + std::to_string(t.movable_entities)
           + ",\"pressure\":" + fmt_double(t.pressure(), 6, true, 3)
           + ",\"free\":" + std::to_string(t.free_slots())
           + ",\"memory_in_use\":" + std::to_string(t.memory_in_use)
           + ",\"total_memory\":" + std::to_string(t.total_memory)
           + ",\"hosted_databases\":" + dbj(t.hosted_databases) + "}";
    };
    for (;;) {
      c.k.fleet_sync();
      NodeTelemetry L = orch.local();
      auto peers = orch.survey();
      uint32_t reach = 0; for (auto& p : peers) if (p.reachable) ++reach;
      std::string j = "{\"hub\":\"" + hub + "\",\"updated_ns\":" + std::to_string(c.k.now())
        + ",\"observer\":\"" + c.k.self().short_str() + "\""
        + ",\"member_count\":" + std::to_string(peers.size())
        + ",\"reachable\":" + std::to_string(reach)
        + ",\"self\":" + tj(L, c.k.self().short_str())
        + ",\"members\":[";
      for (size_t i = 0; i < peers.size(); ++i) {
        if (i) j += ",";
        j += "{\"reachable\":" + std::string(peers[i].reachable ? "true" : "false")
           + ",\"address\":\"" + peers[i].address + "\","
           + "\"telemetry\":" + tj(peers[i].tele, peers[i].tele.kernel.short_str()) + "}";
      }
      j += "]}\n";
#if defined(AIOS_METAL)
      fail(Status::Unsupported, "fleet observe writes its status file to a HOST filesystem; this kernel is the host and has none");
#else
      std::ofstream(path, std::ios::trunc) << j;   // operator status file; not entity state
#endif
      hal::sleep_ms(interval);
    }
  } else if (sub == "slots") {
    if (a.size() > 2) { c.k.set_slots((uint32_t)to_u64(a[2])); c.o << "scheduling capacity set to " << c.k.slots() << " slots\n"; }
    else c.o << "scheduling capacity: " << c.k.slots() << " slots (jobs run concurrently)\n";
  } else if (sub == "watermarks") {
    if (a.size() < 4) fail(Status::Invalid, "fleet watermarks <high> <low>   (fractions of capacity; migrate at >=high, receive at <=low)");
    double hi = 0, lo = 0;
    if (!parse_f64(a[2], hi) || !parse_f64(a[3], lo)) fail(Status::Invalid, "watermarks take two fractions, e.g. fleet watermarks 0.8 0.4");
    if (!orch.set_watermarks(hi, lo))
      fail(Status::Invalid, "watermarks need 0 <= low < high <= 1 (got high " + a[2] + ", low " + a[3] + "): a job moves off at >=high and only to a peer at <=low");
    c.o << "watermarks: high " << pf(orch.high()) << ", low " << pf(orch.low()) << "\n";
  } else if (sub == "plan") {
    c.view();
    c.o << c.o.style(Header()) << "Placement plan" << c.o.style(Reset()) << " (nothing executed)\n";
    fleet_show_decisions(c, orch.plan(), false);
  } else if (sub == "tick") {
    c.o << c.o.style(Header()) << "Placement tick" << c.o.style(Reset()) << "\n";
    fleet_show_decisions(c, orch.tick(), true);
  } else if (sub == "watch") {
    int ticks = a.size() > 2 ? (int)to_u64(a[2]) : 10;
    int interval = a.size() > 3 ? (int)to_u64(a[3]) : 500;
    c.o << c.o.style(Header()) << "Autonomous scheduler" << c.o.style(Reset()) << ": " << ticks << " ticks, " << interval << "ms apart\n";
    for (int i = 0; i < ticks; ++i) {
      auto ds = orch.tick();
      bool acted = false; for (auto& p : ds) if (p.kind != PlacementDecision::Hold) acted = true;
      if (acted) { c.o << "  [tick " << i << "]\n"; fleet_show_decisions(c, ds, true); }
      if (i + 1 < ticks) hal::sleep_ms((uint32_t)interval);
    }
    c.o << "  done — " << orch.log().size() << " decisions on the log\n";
  } else if (sub == "replicate") {
    size_t idx = a.size() > 2 ? (size_t)to_u64(a[2]) : 0;
    auto n = need(orch.replicate_all(idx));
    c.o << "shipped " << n << " recovery point(s) to peer node #" << idx << " — a hard death is now recoverable off-box\n";
  } else if (sub == "replicas") {
    c.view();
    auto reps = c.k.replicas();
    if (reps.empty()) c.o << "  (holding no recovery points)\n";
    for (auto& r : reps) c.o << "  recovery point: job " << r.entity.short_str() << " from kernel " << r.source_kernel.short_str() << " gen " << r.generation << " (" << fmt_bytes(r.bytes) << ")\n";
  } else if (sub == "log") {
    c.view();
    auto& lg = orch.log();
    if (lg.empty()) c.o << "  (no decisions yet)\n";
    for (auto& p : lg) fleet_show_decisions(c, {p}, true);
  } else if (sub == "sql") {
    // fleet sql [writes=on|off] — whether fleet peers may write through this
    // machine's SQL engine (sql_on, ... ON <machine>). Reads always work;
    // writes are refused unless this member turns them on.
    if (has_kv(a, "writes")) {
      std::string v = kv(a, "writes");
      for (auto& ch : v) ch = (char)std::tolower((unsigned char)ch);
      bool on;
      if (v == "on" || v == "1" || v == "yes" || v == "true") on = true;
      else if (v == "off" || v == "0" || v == "no" || v == "false") on = false;
      else fail(Status::Invalid, "fleet sql writes=on|off");
      need(userland::set_fleet_sql_writes(c.k, on));
      // What this kernel already advertises follows the new setting.
      auto dbs = c.k.hosted_databases();
      c.k.clear_hosted_dbs();
      for (auto db : dbs) { db.fleet_read_only = db.read_only || userland::machine_sql_is_fleet_read_only(c.k); c.k.register_hosted_db(db); }
    } else {
      c.view();
    }
    const bool w = userland::fleet_sql_writes_allowed(c.k);
    c.o << "fleet sql: peers may " << (w ? "read and write" : "only read (SELECT, SHOW, DESCRIBE, EXPLAIN)")
        << " through this machine's SQL engine" << (w ? "" : "; `fleet sql writes=on` lets them write") << "\n";
  } else if (sub == "db") {
    // fleet db — discover all databases hosted by fleet peers via telemetry.
    // Polls every connected node; shows db name, peer host, mysql port, table count.
    c.view();
    c.k.fleet_sync();
    auto peers = orch.survey();
    // Also include self
    auto self_dbs = c.k.hosted_databases();
    bool any = false;
    if (!self_dbs.empty()) {
      c.o << c.o.style(Header()) << "Self (" << c.k.self().short_str() << ")" << c.o.style(Reset()) << "\n";
      for (auto& db : self_dbs) {
        c.o << "  " << db.db_name
            << "  asql=" << db.asql_port
            << "  mysql=" << db.mysql_port
            << "  tables=" << db.table_count
            << (db.read_only ? "  [ro: cdc mirror]" : "")
            << (db.fleet_read_only ? "  [fleet sql: read-only]" : "") << "\n";
        any = true;
      }
    }
    for (auto& peer : peers) {
      if (!peer.reachable) continue;
      const auto& dbs = peer.tele.hosted_databases;
      if (dbs.empty()) continue;
      // A relayed peer ("relay:<id>") has no address anyone can dial: its
      // databases are reached with sql_on() / ON <machine>, not a MySQL
      // client. This used to print mysql=relay:<port>.
      const bool relayed = peer.address.rfind("relay:", 0) == 0;
      std::string peer_host = peer.address;
      auto colon = peer_host.rfind(':');
      if (colon != std::string::npos) peer_host = peer_host.substr(0, colon);
      c.o << c.o.style(Header()) << "Peer " << peer.tele.kernel.short_str()
          << " @ " << peer.address << c.o.style(Reset()) << "\n";
      for (auto& db : dbs) {
        c.o << "  " << db.db_name;
        if (db.mysql_port == 0) c.o << "  mysql=none";
        else if (relayed) c.o << "  mysql=port " << db.mysql_port << " (relayed: no dialable address; use sql_on/ON)";
        else c.o << "  mysql=" << peer_host << ":" << db.mysql_port;
        c.o << "  tables=" << db.table_count
            << (db.read_only ? "  [ro: cdc mirror]" : "")
            << (db.fleet_read_only ? "  [fleet sql: read-only]" : "") << "\n";
        any = true;
      }
    }
    if (!any) c.o << "  (no databases discovered — peers may not be in fleet or not running compute serve sql=1)\n";
  } else {
    fail(Status::Invalid, "fleet hub|join|identity|run|members|observe|status|slots|watermarks|plan|tick|watch|replicate|replicas|log|db");
  }
}

void cmd_gateway(Ctx& c, const Args& a) {
  if (a.size() < 2) fail(Status::Invalid, "gateway serve [port=7310] [public=1] [secret=S] [tls=1 cert=PATH key=PATH] [cors=ORIGIN] | gateway token [holder=H] [scope=N] [duration_s=S] | gateway route <req.json> [token=T] | gateway status  (secret: secret= else AIOS_GATEWAY_SECRET else <root>/gateway/secret)");
  const std::string& sub = a[1];
  Orchestrator& orch = orch_for(c.k);
  channel::GatewayConfig cfg;
  // secret= here; the rest of the rule (AIOS_GATEWAY_SECRET, else this root's
  // own key) is resolve_secret()'s, the same one serve() applies, so a token
  // minted here verifies on a gateway started on the same root.
  if (has_kv(a, "secret")) cfg.secret_key = c.r.expand(kv(a, "secret"));
  if (has_kv(a, "cors")) cfg.cors_origin = c.r.expand(kv(a, "cors"));
  if (has_kv(a, "port")) cfg.port = (uint16_t)to_u64(kv(a, "port"));
  if (has_kv(a, "max_depth")) cfg.max_delegation_depth = (uint32_t)to_u64(kv(a, "max_depth"));
  if (has_kv(a, "tls")) cfg.use_tls = (kv(a, "tls") == "1" || kv(a, "tls") == "true");
  if (has_kv(a, "cert")) cfg.cert_file = c.r.expand(kv(a, "cert"));
  if (has_kv(a, "key")) cfg.key_file = c.r.expand(kv(a, "key"));
  if (has_kv(a, "require_custom_secret")) cfg.require_custom_secret = (kv(a, "require_custom_secret") == "1" || kv(a, "require_custom_secret") == "true");

  channel::ControlPlaneGateway gw(c.k, orch, cfg);

  if (sub == "status") {
    c.view();
    NodeTelemetry L = orch.local();
    c.o << c.o.style(Header()) << "Control Plane Gateway" << c.o.style(Reset()) << "\n";
    c.o << "  port: " << cfg.port << " (" << (cfg.use_tls ? "TLS 1.2/1.3" : "HTTP/1.1 + wire RPC") << ")\n";
    c.o << "  local pressure: " << pf(L.pressure()) << " (" << L.free_slots() << " free slots)\n";
    c.o << "  headroom threshold: " << pf(cfg.peer_headroom_threshold) << "\n";
    c.o << "  delegation depth max: " << cfg.max_delegation_depth << "\n";
    auto local_ops = gw.local_kernels();
    c.o << "  registered custom kernels: " << (local_ops.empty() ? "(none)" : "");
    for (size_t i = 0; i < local_ops.size(); ++i) {
      c.o << (i > 0 ? ", " : "") << local_ops[i];
    }
    c.o << "\n";
    return;
  }

  if (sub == "token" || sub == "mint") {
    EntityID holder = c.me.self();
    if (has_kv(a, "holder")) {
      Id128 raw;
      if (Id128::parse(kv(a, "holder"), raw)) holder = EntityID{raw};
    }
    uint64_t scope = has_kv(a, "scope") ? to_u64(kv(a, "scope")) : 100;
    uint64_t duration_s = has_kv(a, "duration_s") ? to_u64(kv(a, "duration_s")) : 3600;
    need(gw.resolve_secret());
    auto tok = gw.mint_token(holder, ResourceKind::ComputeDevice, Right::R_Execute | Right::R_Schedule, scope, duration_s * 1000000000ULL, 0);
    std::string token_str = tok.to_token_string(gw.config().secret_key);
    c.o << token_str << "\n";
    return;
  }

  if (sub == "route") {
    if (a.size() < 3) fail(Status::Invalid, "gateway route <req.json|string> [token=T]");
    std::string req_str = c.r.expand(a[2]);
    if (!req_str.empty() && req_str[0] != '{' && req_str[0] != '[') {
      auto f = c.k.host().read_file(req_str);   // host-io: a file the operator named on the command line
      if (f.ok()) req_str = std::string((const char*)f->data(), f->size());
    }
    auto parsed = channel::rpc_parse(req_str);
    if (!parsed.ok() || parsed->kind() != channel::RpcValue::Kind::Object) fail(Status::Invalid, "request must be valid JSON object");

    std::string tok_str = has_kv(a, "token") ? c.r.expand(kv(a, "token")) : "";
    if (tok_str.empty() && parsed->has("token")) tok_str = (*parsed)["token"].as_text();

    need(gw.resolve_secret());
    channel::GatewayCapabilityToken tok;
    if (!tok_str.empty()) {
      auto parsed_tok = channel::GatewayCapabilityToken::parse_token_string(tok_str, gw.config().secret_key);
      if (parsed_tok.ok()) tok = *parsed_tok;
    }
    auto d = gw.route_request(*parsed, tok, c.k.host().wall_ns());
    c.o << "status: " << channel::route_status_name(d.status) << "\n";
    c.o << "op: " << d.op << "\n";
    c.o << "target: " << d.target_address << " (" << d.target_node_id.str() << ")\n";
    c.o << "cost_units: " << d.cost_units << "\n";
    c.o << "reason: " << d.reason << "\n";
    return;
  }

  if (sub == "serve") {
    bool loopback_only = has_kv(a, "public") ? (kv(a, "public") != "1") : true;
    uint16_t port = has_kv(a, "port") ? (uint16_t)to_u64(kv(a, "port")) : cfg.port;
    hal::Mutex log_mu;
    auto log = [&](const std::string& line) { hal::LockGuard g(log_mu); c.o << line << "\n"; };
    need(gw.resolve_secret());
    c.o << "starting gateway server on port " << port << " (loopback_only=" << (loopback_only ? "1" : "0") << ", tls=" << (cfg.use_tls ? "1" : "0") << ")...\n";
    if (gw.secret_from_root()) c.o << "token secret: this root's own key, " << channel::ControlPlaneGateway::root_secret_path(c.k) << "\n";
    need(gw.serve(port, loopback_only, nullptr, log));
    return;
  }

  fail(Status::Invalid, "gateway serve|token|route|status");
}

void show_cap(Ctx& c, const Capability& cap) {
  c.o << cap.id.short_str() << "  holder=" << cap.holder.short_str() << "  " << W{22} << cap.resource.str() << " " << rights_str(cap.rights);
  if (cap.scope_limit && cap.resource.kind == ResourceKind::Kernel) c.o << "  uses=" << fmt_u64(cap.scope_used) << "/" << fmt_u64(cap.scope_limit);
  else if (cap.scope_limit) c.o << "  scope=" << fmt_bytes(cap.scope_used) << "/" << fmt_bytes(cap.scope_limit);
  if (cap.expires_ns) c.o << "  expires=" << (cap.expires_ns > c.k.now() ? "+" + fmt_ns(cap.expires_ns - c.k.now()) : "EXPIRED");
  if (!cap.parent.is_null()) c.o << "  from=" << cap.parent.short_str();
  c.o << "\n";
}

// authority — the console's own standing, and the kernel's record of every
// decision about authority at this root (persistent, replayed at boot).
void cmd_authority(Ctx& c, const Args& a) {
  std::string sub = a.size() > 1 ? a[1] : "mine";
  if (sub == "mine") {
    { const Entity* e = c.k.entities().get(c.me.self()); c.o << "entity " << c.me.self().str() << (e ? " (" + e->label + ")" : "") << (c.k.is_resident(c.me.self()) ? "  — resident: persists across boots" : "") << "\n"; }
    for (auto& cap : c.me.authority()) show_cap(c, cap);
    c.o << "not held over kernel:*: " << rights_str((uint16_t)(R_All & ~[&]{ uint16_t r = 0; for (auto& cap : c.me.authority()) if (cap.resource.kind == ResourceKind::Kernel && cap.resource.id.is_null()) r |= cap.rights; return r; }())) << "  — kernel ownership stays with the kernel entity " << c.k.self().short_str() << "\n";
  } else if (sub == "residents") {
    c.view();
    for (auto& r : c.k.authority().residents()) c.o << r.id.str() << "  " << r.label << "\n";
  } else if (sub == "log") {
    c.view();
    uint64_t n = a.size() > 2 ? to_u64(a[2]) : 20;
    auto& ev = c.k.authority().events();
    size_t start = ev.size() > n ? ev.size() - n : 0;
    for (size_t i = start; i < ev.size(); ++i) {
      auto& e = ev[i];
      c.o << W{10} << authority_event_name(e.kind) << " actor=" << e.actor.short_str() << " subject=" << e.subject.short_str() << " " << e.resource.str() << " " << rights_str(e.rights);
      if (!e.capability.is_null()) c.o << " cap=" << e.capability.short_str();
      if (!e.note.empty()) c.o << "  " << e.note;
      c.o << "\n";
    }
    if (ev.empty()) c.o << "(no authority events recorded)\n";
  } else if (sub == "refusals") {
    c.view();
    size_t n = 0;
    for (auto& e : c.k.authority().events()) if (e.kind == AuthorityEvent::Refused) { n++; c.o << "REFUSED actor=" << e.actor.short_str() << " wanted " << e.resource.str() << " " << rights_str(e.rights) << " for " << e.subject.short_str() << "  " << e.note << "\n"; }
    if (!n) c.o << "the kernel has refused nothing at this root\n";
  } else fail(Status::Invalid, "authority [mine|residents|log [n]|refusals]");
}

void cmd_expect(Ctx& c, const Args& a) {
  // expect <text-in-$read> — asserts the last read payload contains the text.
  if (a.size() < 2) fail(Status::Invalid, "expect <substring>");
  auto it = c.s.vars.find("read");
  std::string have = it == c.s.vars.end() ? "" : it->second;
  std::string want = c.r.expand(a[1]);
  if (have.find(want) == std::string::npos) fail(Status::Conflict, "expected to read \"" + want + "\" but read \"" + printable(span_of(have)) + "\"");
  c.o << "ok: read contains \"" << want << "\"\n";
}

void cmd_expect_error(Ctx& c, const Args& a) {
  if (a.size() < 3) fail(Status::Invalid, "expect-error <substring> <cmd...>");
  std::string want = c.r.expand(a[1]);
  std::string cmd_line;
  for (size_t i = 2; i < a.size(); ++i) {
    if (i > 2) cmd_line += " ";
    cmd_line += c.r.expand(a[i]);
  }
  size_t old_failures = c.s.failures;
  auto r = run_line(c.s, cmd_line);
  if (r.ok()) {
    c.s.failures = old_failures + 1;
    fail(Status::Conflict, "expected command to fail with \"" + want + "\" but it succeeded");
  }
  c.s.failures = old_failures;
  if (r.error().str().find(want) == std::string::npos) {
    c.s.failures = old_failures + 1;
    fail(Status::Conflict, "expected error to contain \"" + want + "\" but got: " + r.error().str());
  }
  c.o << "ok: error contains \"" << want << "\"\n";
}


}  // namespace

std::vector<std::string> split(const std::string& str, char sep) {
  std::vector<std::string> out; std::string cur;
  for (char ch : str) { if (ch == sep) { out.push_back(cur); cur.clear(); } else cur.push_back(ch); }
  out.push_back(cur);
  if (!out.empty() && out.back().empty()) out.pop_back();
  return out;
}
bool parse_u64(const std::string& str, uint64_t& out) {
  size_t i = 0; int base = 10; out = 0;
  if (str.size() > 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) { base = 16; i = 2; }
  if (i >= str.size()) return false;
  for (; i < str.size(); ++i) {
    char c = str[i]; int d;
    if (c >= '0' && c <= '9') d = c - '0'; else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10; else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10; else return false;
    out = out * (uint64_t)base + (uint64_t)d;
  }
  return true;
}
bool parse_f64(const std::string& str, double& out) {
  size_t i = 0; bool neg = false; out = 0;
  if (i < str.size() && (str[i] == '-' || str[i] == '+')) { neg = str[i] == '-'; ++i; }
  bool any = false; double v = 0;
  while (i < str.size() && str[i] >= '0' && str[i] <= '9') { v = v * 10 + (str[i] - '0'); ++i; any = true; }
  if (i < str.size() && str[i] == '.') { ++i; double f = 0.1; while (i < str.size() && str[i] >= '0' && str[i] <= '9') { v += (str[i] - '0') * f; f /= 10; ++i; any = true; } }
  if (!any) return false;
  if (i < str.size() && (str[i] == 'e' || str[i] == 'E')) {
    ++i; bool eneg = false; if (i < str.size() && (str[i] == '-' || str[i] == '+')) { eneg = str[i] == '-'; ++i; }
    int e = 0; bool eany = false; while (i < str.size() && str[i] >= '0' && str[i] <= '9') { e = e * 10 + (str[i] - '0'); ++i; eany = true; }
    if (!eany) return false;
    for (int k = 0; k < e; ++k) v = eneg ? v / 10 : v * 10;
  }
  if (i != str.size()) return false;
  out = neg ? -v : v; return true;
}

// Whatever this machine was told to do on a schedule, done — with the authority
// of the entity that was named when the order was given, not the console's.
size_t run_due_directives(Session& s, bool announce) {
  // A directive that runs directives is not a plan.
  static bool inside = false;
  if (inside || !s.kernel || !s.me) return 0;
  // The metal console asks this thousands of times a second between keystrokes.
  // Looking at the clock is cheap; scanning the kernel's states and reading the
  // register off disk is not, and doing it per poll is what turns an idle
  // machine into a busy one.
  // Directive due times are wall-clock milliseconds, so a saved schedule means
  // the same thing after a reboot (a since-boot clock left them far in the future).
  uint64_t now = s.kernel->host().wall_ns() / 1000000ull;
  static uint64_t looked_at = 0;
  if (now < looked_at + 250) return 0;
  looked_at = now;
  userland::Directives reg(*s.me);
  if (!reg.attach(false).ok()) return 0;
  auto due = reg.due(now);
  if (due.empty()) return 0;
  // Something is due, so now it matters whether the entity that was told to do
  // it is still here. A process that did not give the order does not have it —
  // the register kept its checkpoint, and this is the moment to spend the cost
  // of bringing the identity back. Asking on every idle poll instead would put
  // a kernel-wide entity scan in the machine's idle path for nothing.
  reg.restore_missing();

  // And what the machine has LEARNED since this process booted. The object
  // store is shared — the desktop writes what it dreams, this process reads it
  // — and nothing outside the desktop's own frame loop ever called refresh(),
  // so a standing directive ran against the store as it stood when the shell
  // started. Every `dream-<name>` directive for a dream created since then
  // failed with "this machine has no dream called <name>": 28 of them in this
  // machine's session log, which is the whole of its self-improvement loop,
  // silently off.
  (void)s.kernel->store().refresh();

  inside = true;
  size_t ran = 0;
  for (const auto& d : due) {
    std::string line = d.line;
    // The directive runs WITH d.as's authority: the session's acting surface is
    // that entity for the length of the line. (It used to append " as=<id>" to
    // the command text, so `echo tick > f` wrote "tick as=…" into f.)
    std::unique_ptr<Surface> acting;
    if (!d.as.is_null() && d.as != s.me->self()) {
      auto as = s.me->act_as(d.as);
      if (!as.ok()) {
        reg.note_run(d.name, s.kernel->host().wall_ns() / 1000000ull, 0, false,
                     "cannot act as " + d.as.short_str() + ": " + as.error().str());
        ++ran;
        continue;
      }
      acting = std::make_unique<Surface>(std::move(*as));
    }
    struct SwapBack {
      std::unique_ptr<Surface>& me;
      std::unique_ptr<Surface>& other;
      bool on;
      SwapBack(std::unique_ptr<Surface>& m, std::unique_ptr<Surface>& o) : me(m), other(o), on(o != nullptr) {
        if (on) std::swap(me, other);
      }
      ~SwapBack() { if (on) std::swap(me, other); }
    };
    std::string captured;
    Out* previous = s.out;
    // Capture for the one-line summary below, but ALSO stream it to wherever the
    // machine is talking (the session log, the console) as it happens. A person
    // watching should see a standing order being carried out — the pages it
    // reads, the mind thinking, what it decided — not just a summary afterwards.
    Out sink([&](const std::string& t) { captured += t; if (announce && previous) { *previous << t; previous->flush(); } });
    sink.set_colour(false);
    s.out = &sink;
    uint64_t started = s.kernel->host().monotonic_ns();
    Result<void> r = Ok();
    {
      SwapBack as_directive(s.me, acting);
      r = run_line(s, line);
    }
    sink.flush();
    s.out = previous;
    uint64_t took = (s.kernel->host().monotonic_ns() - started) / 1000000ull;

    std::string first;
    for (size_t at = 0; at <= captured.size() && first.empty();) {
      size_t nl = captured.find('\n', at);
      std::string one = captured.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
      // The first line that says something, with the colouring stripped.
      std::string clean;
      for (size_t j = 0; j < one.size(); ++j) {
        if (one[j] == 27) { while (j < one.size() && !(one[j] >= 0x40 && one[j] <= 0x7E)) ++j; continue; }
        clean += one[j];
      }
      while (!clean.empty() && clean.front() == ' ') clean.erase(clean.begin());
      if (!clean.empty()) first = clean;
      if (nl == std::string::npos) break;
      at = nl + 1;
    }
    uint64_t finished = s.kernel->host().wall_ns() / 1000000ull;
    reg.note_run(d.name, finished, took, r.ok(), r.ok() ? first : r.error().str());
    ++ran;
    if (announce && s.out) {
      Out& o = *s.out;
      o << o.style(r.ok() ? Green() : Red()) << "[" << d.name << "]" << o.style(Reset()) << " "
        << (r.ok() ? (first.size() > 96 ? first.substr(0, 96) + "…" : first) : r.error().str())
        << o.style(Dim()) << "  (" << took << " ms, as " << d.as.short_str() << ")" << o.style(Reset()) << "\n";
      o.flush();
    }
  }
  inside = false;
  return ran;
}

std::vector<std::string> tokenize(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool in_q = false, any = false;
  char q = 0;
  for (size_t i = 0; i < line.size(); ++i) {
    char ch = line[i];
    if (in_q) {
      if (ch == q) in_q = false;
      // A backslash escapes only the quote and itself (as a shell's double
      // quotes do); before anything else it is kept, so an SQL string's own
      // escapes - CALL SAT('p cnf 2 1\n1 2 0'), 'it\'s' - reach the engine.
      // It used to be dropped, and \n arrived as a bare 'n'.
      else if (ch == '\\' && i + 1 < line.size() && (line[i + 1] == q || line[i + 1] == '\\')) cur.push_back(line[++i]);
      else cur.push_back(ch);
    } else if (ch == '"' || ch == '\'') { in_q = true; q = ch; any = true; }
    else if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') { if (!cur.empty() || any) { out.push_back(cur); cur.clear(); any = false; } }
    else if (ch == '#' && cur.empty()) break;
    else cur.push_back(ch);
  }
  if (!cur.empty() || any) out.push_back(cur);
  return out;
}

void print_help(Out& o) {
  o << o.style(Header()) << "AIOS Command Surface — System Capabilities & Console Commands" << o.style(Reset()) << "\n\n";

  o << o.style(Bold()) << "SYSTEM & ENVIRONMENT" << o.style(Reset()) << "\n"
    << "  " << o.style(Cyan()) << W{32} << "info" << o.style(Reset()) << "Display OS topology, memory fabric, and entity summary\n"
    << "  " << o.style(Cyan()) << W{32} << "sysstat" << o.style(Reset()) << "Display detailed system statistics and health dashboard\n"
    << "  " << o.style(Cyan()) << W{32} << "devices" << o.style(Reset()) << "List compute devices (CPU, GPU, NPU, Remote)\n"
    << "  " << o.style(Cyan()) << W{32} << "storage" << o.style(Reset()) << "List storage devices and root volume mount point\n"
    << "  " << o.style(Cyan()) << W{32} << "network" << o.style(Reset()) << "List network interfaces, IP addresses, and link status\n"
    << "  " << o.style(Cyan()) << W{32} << "whoami | id" << o.style(Reset()) << "Show active console entity identity\n"
    << "  " << o.style(Cyan()) << W{32} << "identity" << o.style(Reset()) << "Display or inspect system identity object\n"
    << "  " << o.style(Cyan()) << W{32} << "subsystems [-v]" << o.style(Reset()) << "Query hardware subsystem capabilities (-v for device names)\n"
    << "  " << o.style(Cyan()) << W{32} << "bus list|peer <src> <dst>" << o.style(Reset()) << "List PCI devices/windows or copy data between peer devices\n"
    << "  " << o.style(Cyan()) << W{32} << "attest status|pcrs|quote" << o.style(Reset()) << "TPM attestation: query status, PCR values, and signed quotes\n"
    << "  " << o.style(Cyan()) << W{32} << "watchdog" << o.style(Reset()) << "System watchdog: monitor health checks, heap, and recovery status\n"
    << "  " << o.style(Cyan()) << W{32} << "load" << o.style(Reset()) << "Measure system load and request compute quota from host\n"
    << "  " << o.style(Cyan()) << W{32} << "reboot | poweroff" << o.style(Reset()) << "Restart or shut down the system substrate\n"
    << "  " << o.style(Cyan()) << W{32} << "sleep <sec>" << o.style(Reset()) << "Pause execution for specified seconds\n"
    << "  " << o.style(Cyan()) << W{32} << "quit | exit" << o.style(Reset()) << "Exit the command surface session\n\n";

  o << o.style(Bold()) << "AUTHORITY & SECURITY" << o.style(Reset()) << "\n"
    << "  " << o.style(Cyan()) << W{32} << "authority [view]" << o.style(Reset()) << "Show capability profile and audit log (mine, residents, log, refusals)\n"
    << "  " << o.style(Cyan()) << W{32} << "cap grant <e> <kind> <id> <r>" << o.style(Reset()) << "Grant derived capability rights to an entity\n"
    << "  " << o.style(Cyan()) << W{32} << "cap request <kind> <id> <r>" << o.style(Reset()) << "Request capability rights from kernel\n"
    << "  " << o.style(Cyan()) << W{32} << "quota set|list|clear|reset" << o.style(Reset()) << "Manage execution time and compute quotas per entity\n"
    << "  " << o.style(Cyan()) << W{32} << "gateway serve|token|mint|route" << o.style(Reset()) << "Token-gated HTTP gateway for remote compute requests\n"
    << "  " << o.style(Cyan()) << W{32} << "cap delegate|revoke|list" << o.style(Reset()) << "Manage capabilities (delegate, revoke, or list by kind)\n\n";

  o << o.style(Bold()) << "ENTITIES & MEMORY FABRIC" << o.style(Reset()) << "\n"
    << "  " << o.style(Cyan()) << W{32} << "entity create|list|show" << o.style(Reset()) << "Manage persistent system entities\n"
    << "  " << o.style(Cyan()) << W{32} << "mem alloc|read|write" << o.style(Reset()) << "Allocate, read, or write memory regions (system/shared/persistent/device)\n"
    << "  " << o.style(Cyan()) << W{32} << "mem migrate|share|list" << o.style(Reset()) << "Migrate, share, or list allocated memory objects\n"
    << "  " << o.style(Cyan()) << W{32} << "state create|set|get|list" << o.style(Reset()) << "Manage entity state objects\n"
    << "  " << o.style(Cyan()) << W{32} << "kv set|get|del|list" << o.style(Reset()) << "In-memory key-value store backed by StateObjects\n\n";

  o << o.style(Bold()) << "COMPUTE & GRAPH EXECUTION" << o.style(Reset()) << "\n"
    << "  " << o.style(Cyan()) << W{32} << "tensor create|show" << o.style(Reset()) << "Create or inspect typed tensor views (f32, f64, i32, etc.)\n"
    << "  " << o.style(Cyan()) << W{32} << "compute kernels|serve" << o.style(Reset()) << "List compute kernels or serve compute/SQL service over network\n"
    << "  " << o.style(Cyan()) << W{32} << "sat solve <file>" << o.style(Reset()) << "Boolean satisfiability (SAT) solver: find model or prove UNSAT\n"
    << "  " << o.style(Cyan()) << W{32} << "graph create|op|plan|run" << o.style(Reset()) << "Build, optimize, schedule, and execute compute graphs\n\n";

  o << o.style(Bold()) << "CHANNELS & OBJECT STORE" << o.style(Reset()) << "\n"
    << "  " << o.style(Cyan()) << W{32} << "channel create|pub|sub|list" << o.style(Reset()) << "Manage IPC state channels and ring buffers (pub/sub)\n"
    << "  " << o.style(Cyan()) << W{32} << "sql query|serve|import|dump" << o.style(Reset()) << "ASQL relational database: execute queries, run MySQL server, import/dump\n"
    << "  " << o.style(Cyan()) << W{32} << "ingress receive|verify|list" << o.style(Reset()) << "Receive, verify, and inspect incoming network payloads\n"
    << "  " << o.style(Cyan()) << W{32} << "feed ingest <url>|list" << o.style(Reset()) << "Ingest and list RSS/Atom feeds into semantic object store\n"
    << "  " << o.style(Cyan()) << W{32} << "store put|get|similar|list" << o.style(Reset()) << "Semantic object store: store, retrieve, search similarity, or list\n\n";

  o << o.style(Bold()) << "INTELLIGENCE & DISTRIBUTED" << o.style(Reset()) << "\n"
    << "  " << o.style(Cyan()) << W{32} << "ask <request>" << o.style(Reset()) << "Submit natural language task or prompt to resident AI model\n"
    << "  " << o.style(Cyan()) << W{32} << "mind bind|cycle|unbind" << o.style(Reset()) << "Manage AI backend slot binding (echo, plugin, extern, wire, qwen, jepa)\n"
    << "  " << o.style(Cyan()) << W{32} << "infer load|status|ask|serve" << o.style(Reset()) << "Run local Qwen2 checkpoint or serve OpenAI-compatible completions API\n"
    << "  " << o.style(Cyan()) << W{32} << "jepa status|record|train|tick" << o.style(Reset()) << "JEPA world model: inspect latent state, record transitions, train policy\n"
    << "  " << o.style(Cyan()) << W{32} << "fleet hub|join|members|status" << o.style(Reset()) << "Cluster fleet management: hub discovery, node membership, load balancing\n"
    << "  " << o.style(Cyan()) << W{32} << "p2p status|peers|ping|mdns" << o.style(Reset()) << "Peer-to-peer networking: local DHT, mDNS discovery, and node ping\n"
    << "  " << o.style(Cyan()) << W{32} << "qwen status|ask|local" << o.style(Reset()) << "Query remote LLM model server or local in-tree model\n"
    << "  " << o.style(Cyan()) << W{32} << "lm status|ask|train|tokenize" << o.style(Reset()) << "Built-in small language model: status, generation, training, tokenizer\n"
    << "  " << o.style(Cyan()) << W{32} << "voice say|listen|status" << o.style(Reset()) << "Text-to-speech (TTS) synthesis and speech recognition (STT)\n"
    << "  " << o.style(Cyan()) << W{32} << "node serve|connect|memory|list" << o.style(Reset()) << "Connect and manage remote peer kernels across network fabric\n"
    << "  " << o.style(Cyan()) << W{32} << "checkpoint <e> | restore <path>" << o.style(Reset()) << "Create or restore entity state snapshots\n\n";

  o << o.style(Bold()) << "APPLICATIONS & SHELL" << o.style(Reset()) << "\n"
    << "  " << o.style(Cyan()) << W{32} << "display show|set|status|list" << o.style(Reset()) << "Display manager: inspect framebuffer, resolution, and active views\n"
    << "  " << o.style(Cyan()) << W{32} << "scene" << o.style(Reset()) << "Inspect and manage desktop scene graph and layout\n"
    << "  " << o.style(Cyan()) << W{32} << "keys list|bind|unbind|reset" << o.style(Reset()) << "Manage input key bindings and shortcuts\n"
    << "  " << o.style(Cyan()) << W{32} << "app list|install|open|stop" << o.style(Reset()) << "Manage and launch installed applications\n"
    << "  " << o.style(Cyan()) << W{32} << "js <program>|features" << o.style(Reset()) << "Execute JavaScript code using built-in ECMAScript engine\n"
    << "  " << o.style(Cyan()) << W{32} << "face reference|look|text|source" << o.style(Reset()) << "Face UI language: view language reference, render UI, or inspect source\n"
    << "  " << o.style(Cyan()) << W{32} << "mail sync|list|search|read|send" << o.style(Reset()) << "Mail client: synchronize accounts, search, read, and compose email\n"
    << "  " << o.style(Cyan()) << W{32} << "google auth|status|sync|watch|logout" << o.style(Reset()) << "Sign in to a Google account (OAuth 2.0) and keep its mail, calendar, contacts and notes in sync\n"
    << "  " << o.style(Cyan()) << W{32} << "contacts list|search|add|show" << o.style(Reset()) << "Address book and contact management\n"
    << "  " << o.style(Cyan()) << W{32} << "calendar agenda|add|show|list" << o.style(Reset()) << "Calendar and agenda event management\n"
    << "  " << o.style(Cyan()) << W{32} << "task create|list|show|update" << o.style(Reset()) << "Task ledger and todo tracking\n"
    << "  " << o.style(Cyan()) << W{32} << "chain | btc peers|headers|rpc" << o.style(Reset()) << "Bitcoin client: manage P2P peers, query headers, and execute JSON-RPC\n"
    << "  " << o.style(Cyan()) << W{32} << "wallet info|send|history|import" << o.style(Reset()) << "Crypto wallet: view balances, sign transactions, import private keys\n"
    << "  " << o.style(Cyan()) << W{32} << "contract create|solve|watch" << o.style(Reset()) << "Manage and solve Hash Time-Locked Contracts (HTLC)\n"
    << "  " << o.style(Cyan()) << W{32} << "tx plan|stage|commit|abort|show" << o.style(Reset()) << "Atomic multi-file patch transactions (plan, stage, commit, abort)\n"
    << "  " << o.style(Cyan()) << W{32} << "gui [desktop [--tui]]" << o.style(Reset()) << "AIOS desktop environment: start GUI/TUI interface or show options\n"
    << "  " << o.style(Cyan()) << W{32} << "browser <url>|follow <n>|back" << o.style(Reset()) << "Built-in web browser: fetch, render, and navigate web pages\n"
    << "  " << o.style(Cyan()) << W{32} << "daemon list|start|status" << o.style(Reset()) << "Manage background userland services and daemons\n"
    << "  " << o.style(Cyan()) << W{32} << "build [test|gate|<args...>]" << o.style(Reset()) << "Compile system targets, execute test suites, or run audit gates\n"
    << "  " << o.style(Cyan()) << W{32} << "git init|add|commit|log|status" << o.style(Reset()) << "Native Git client: repository initialization, commits, and logs\n"
    << "  " << o.style(Cyan()) << W{32} << "git branch|switch <rev>" << o.style(Reset()) << "Manage, list, and switch repository branches\n"
    << "  " << o.style(Cyan()) << W{32} << "git repos|use|fsck [as=$entity]" << o.style(Reset()) << "Manage registered Git repositories and check repository integrity\n"
    << "  " << o.style(Cyan()) << W{32} << "git clone|fetch|pull|push" << o.style(Reset()) << "Synchronize Git repositories over SSH and Git protocols\n"
    << "  " << o.style(Cyan()) << W{32} << "ssh <user@host> <cmd> | keygen" << o.style(Reset()) << "Native SSH-2 client and keypair generation utility\n"
    << "  " << o.style(Cyan()) << W{32} << "npm install|view <package>" << o.style(Reset()) << "Install and inspect npm packages directly from npm registry\n"
    << "  " << o.style(Cyan()) << W{32} << "pip install|view <package>" << o.style(Reset()) << "Install and inspect Python packages from PyPI with SHA-256 verification\n"
    << "  " << o.style(Cyan()) << W{32} << "cargo vendor|view <crate>" << o.style(Reset()) << "Vendor and inspect Rust crates from crates.io with checksum checks\n"
    << "  " << o.style(Cyan()) << W{32} << "go get|list|mod verify <module>" << o.style(Reset()) << "Download, list, and verify Go modules against checksum database\n"
    << "  " << o.style(Cyan()) << W{32} << "elf <path>" << o.style(Reset()) << "Inspect ELF binary headers, sections, and program segments\n"
    << "  " << o.style(Cyan()) << W{32} << "sense <url>" << o.style(Reset()) << "Fetch web page via TLS and extract content into semantic store\n"
    << "  " << o.style(Cyan()) << W{32} << "dream <what you want>" << o.style(Reset()) << "Synthesize and render an application or UI from natural language prompt\n"
    << "  " << o.style(Cyan()) << W{32} << "dream list|show|open|remove" << o.style(Reset()) << "Manage synthesized applications: list, inspect, open, or delete\n"
    << "  " << o.style(Cyan()) << W{32} << "organ grow|call|list|source" << o.style(Reset()) << "Dynamically synthesize, compile, load, and execute C++ extensions\n"
    << "  " << o.style(Cyan()) << W{32} << "grain run|eval|dis|repl" << o.style(Reset()) << "Grain language runtime: evaluate code, disassemble bytecode, or start REPL\n"
    << "  " << o.style(Cyan()) << W{32} << "horizon scan|show|distil|brief" << o.style(Reset()) << "Scan external news and feeds, distilling briefings relevant to current tasks\n"
    << "  " << o.style(Cyan()) << W{32} << "directive add|list|remove" << o.style(Reset()) << "Manage background directives executed during idle cycles\n"
    << "  " << o.style(Cyan()) << W{32} << "edit <file> <old> <new>" << o.style(Reset()) << "Find and replace text within a file in place\n"
    << "  " << o.style(Cyan()) << W{32} << "host <command...>" << o.style(Reset()) << "Execute command on the underlying host operating system\n"
    << "  " << o.style(Cyan()) << W{32} << "install dualboot|disk|status" << o.style(Reset()) << "Bare-metal installer: manage disk partitioning and dual-boot setup\n"
    << "  " << o.style(Cyan()) << W{32} << "set NAME" << o.style(Reset()) << "Bind last produced command output to session variable $NAME\n"
    << "  " << o.style(Cyan()) << W{32} << "echo <text>" << o.style(Reset()) << "Print text to standard output\n"
    << "  " << o.style(Cyan()) << W{32} << "expect <text>" << o.style(Reset()) << "Assertion test: fail unless previous output contains specified text\n"
    << "  " << o.style(Cyan()) << W{32} << "rewind <path>" << o.style(Reset()) << "Restore a running entity to a saved checkpoint image\n"
    << "  " << o.style(Cyan()) << W{32} << "help" << o.style(Reset()) << "Display this command reference list\n"
    << "  " << o.style(Cyan()) << W{32} << "pwd · cd · ls · cat · grep..." << o.style(Reset()) << "POSIX compatibility shell builtins (`man` for full list)\n";
}

void cmd_miner(Ctx& c, const Args& a) {
  c.view();
  userland::BitcoinMiner miner(c.k);
  need(miner.init());

  std::string sub = (a.size() > 1) ? a[1] : "status";
  const char* const not_mining =
      "a local SHA-256d hashing exercise: it hashes a header no network would accept and credits nothing";

  if (sub == "status" || sub == "info") {
    const auto& st = miner.state();
    auto hw = miner.inspect_hardware();
    c.o << c.o.style(Header()) << "Local hashing exercise (SHA-256d)" << c.o.style(Reset()) << "\n";
    c.o << "  " << W{20} << "What it is" << ": " << not_mining << "\n";
    c.o << "  " << W{20} << "Real mining" << ": " << c.o.style(Yellow()) << "none — no pool, no network work, no rewards"
        << c.o.style(Reset()) << " (the network's real target: 'chain mine')\n";
    c.o << "  " << W{20} << "Lanes" << ": " << hw.cpus << " (" << hw.cpu_arch << ")\n";
    c.o << "  " << W{20} << "Total hashes" << ": " << st.total_hashes << "\n";
    if (st.last_run_ns > 0 && st.last_run_hashes > 0) {
      c.o << "  " << W{20} << "Last run" << ": " << st.last_run_hashes << " hashes in " << fmt_ns(st.last_run_ns)
          << " on " << st.last_run_threads << " thread(s)\n";
      c.o << "  " << W{20} << "Measured rate" << ": " << fmt_double(miner.measured_hashrate(), 12, true, 1) << " H/s\n";
    } else {
      c.o << "  " << W{20} << "Measured rate" << ": none — nothing has been hashed in this root yet\n";
    }
    if (!st.best_hash_hex.empty())
      c.o << "  " << W{20} << "Best hash" << ": " << st.best_hash_hex << " (" << st.best_zero_bits << " leading zero bits)\n";
    c.o << "  " << W{20} << "At difficulty 1" << ": " << st.total_meeting_target << " hash(es) met it (not blocks)\n";
    if (st.legacy_invented_blocks)
      c.o << "  " << c.o.style(Yellow()) << "note" << c.o.style(Reset()) << ": this root's miner.state lists "
          << st.legacy_invented_blocks << " \"blocks mined\" written by an earlier version against a made-up easy "
          << "target. They were never blocks and earned nothing; the next run rewrites the file without them.\n";
  } else if (sub == "inspect-hw" || sub == "hw") {
    auto hw = miner.inspect_hardware();
    c.o << c.o.style(Header()) << "Hashing lanes" << c.o.style(Reset()) << "\n";
    c.o << "  " << W{20} << "Logical CPUs" << ": " << hw.cpus << "\n";
    c.o << "  " << W{20} << "Memory" << ": " << fmt_bytes(hw.memory_bytes) << "\n";
    c.o << "  " << W{20} << "CPU architecture" << ": " << hw.cpu_arch << "\n";
    c.o << "  " << W{20} << "Split" << ": nonces strided across " << hw.cpus << " thread(s)\n";
  } else if (sub == "work") {
    auto t = miner.local_template();
    c.o << c.o.style(Header()) << "Local template (not network work)" << c.o.style(Reset()) << "\n";
    c.o << "  " << W{20} << "Header" << ": " << crypto::bytes_to_hex(ByteSpan(t.header.data(), 76)) << "<nonce>\n";
    c.o << "  " << W{20} << "Previous block" << ": none (all zero) — there is no chain behind it\n";
    char bits_hex[12];
    std::snprintf(bits_hex, sizeof bits_hex, "%08x", t.bits);
    c.o << "  " << W{20} << "nBits" << ": " << bits_hex << " (difficulty 1)\n";
    c.o << "  " << W{20} << "Target" << ": " << t.target_hex << "\n";
    c.o << "  " << W{20} << "Reward" << ": none — a hash under this target is not a block\n";
    c.o << "  for the network's real target and this machine's odds against it: chain mine [seconds=N]\n";
  } else if (sub == "mine" || sub == "hash" || sub == "run") {
    // miner mine [rounds] [threads] [nonces=N]
    std::vector<std::string> pos;
    for (size_t i = 2; i < a.size(); ++i) if (a[i].find('=') == std::string::npos) pos.push_back(a[i]);
    uint64_t rounds = pos.size() > 0 ? to_u64(pos[0]) : 1;
    uint32_t threads = has_kv(a, "threads") ? (uint32_t)to_u64(kv(a, "threads"))
                                            : (pos.size() > 1 ? (uint32_t)to_u64(pos[1]) : 0);
    uint64_t nonces = has_kv(a, "nonces") ? to_u64(kv(a, "nonces")) : 10000000;
    if (pos.size() > 2 && pos[2] != "btc" && pos[2] != "BTC")
      fail(Status::Unsupported, "'" + pos[2] + "': only SHA-256d is implemented here (no RandomX, so no Monero)");
    if (rounds == 0) rounds = 1;
    c.o << "Hashing " << rounds << " round(s) of " << nonces << " nonces — " << not_mining << "\n";
    for (uint64_t i = 0; i < rounds; ++i) {
      auto run = need(miner.hash_run(threads, nonces));
      double rate = run.ns ? (double)run.hashes * 1e9 / (double)run.ns : 0.0;
      c.o << "  round " << (i + 1) << ": " << run.hashes << " hashes in " << fmt_ns(run.ns) << " on " << run.threads
          << " thread(s), " << fmt_double(rate / 1e6, 3, true, 3) << " MH/s; best " << run.best_hash_hex.substr(0, 16)
          << "… (" << run.best_zero_bits << " zero bits); " << run.meeting_target << " under difficulty 1\n";
    }
    c.o << "  credited: nothing (no block, no pool, no network)\n";
  } else if (sub == "coin") {
    if (a.size() > 2 && a[2] != "btc" && a[2] != "BTC" && a[2] != "bitcoin")
      fail(Status::Unsupported, "only SHA-256d is implemented here; Monero needs RandomX, which this machine does not have");
    c.o << "SHA-256d (the only hash implemented)\n";
  } else if (sub == "wallet") {
    c.o << "the miner pays nothing to anyone, so it has no wallet. Keys: 'wallet'; payout balances: 'chain balance'\n";
  } else if (sub == "daemon" || sub == "start") {
    fail(Status::Unsupported,
         "there is no background miner: 'miner mine' hashes in the foreground and stops when it is done");
  } else if (sub == "stop") {
    // An earlier version spawned a 'daemon:miner' entity that did nothing.
    EntityID target;
    for (auto& e : c.k.entities().list())
      if (e.label == "daemon:miner" && e.status != EntityStatus::Terminated) { target = e.id; break; }
    if (!target.is_null()) {
      need(c.me.terminate(target));
      c.o << "removed the leftover 'daemon:miner' entity (" << target.short_str() << "); it never mined anything\n";
    } else {
      c.o << "no miner is running (there is no background miner)\n";
    }
  } else if (sub == "reboot-resume") {
    // Called at every kernel start (main.cpp). There is no background miner to
    // bring back, so this does nothing.
  } else {
    fail(Status::Invalid, "miner status | work | mine [rounds] [threads] [nonces=N] | hw   (a local hashing exercise; credits nothing)");
  }
}

namespace chain = userland::chain;

chain::Network chain_network_arg(const Args& a, size_t from) {
  for (size_t i = from; i < a.size(); ++i) {
    auto n = chain::network_by_name(a[i]);
    if (n.ok()) return *n;
  }
  return chain::Network::Mainnet;
}

std::array<uint8_t, 32> chain_genesis(chain::Network net) {
  const char* display = "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f";
  if (net == chain::Network::Testnet) {
    display = "000000000933ea01ad0ee984209779baaec3ced90fa3f408719526f8d77f4943";
  } else if (net == chain::Network::Signet) {
    display = "00000008819873e925422c1ff0f99f7cc9bbb232af63a077a480a3633bee1ef6";
  }
  return *chain::hash_from_display(display);
}

void cmd_chain(Ctx& c, const Args& a) {
  c.view();
  std::string sub = (a.size() > 1) ? a[1] : "peers";
  chain::Network net = chain_network_arg(a, 2);
  const auto& p = chain::network_params(net);

  if (sub == "peers" || sub == "peer") {
    uint64_t want = has_kv(a, "count") ? to_u64(kv(a, "count")) : 3;
    auto seeds = channel::dns_seeds(p.magic);
    c.o << c.o.style(Header()) << "Bitcoin " << p.name << " peers" << c.o.style(Reset()) << "\n";

    uint64_t reached = 0;
    for (const auto& seed : seeds) {
      if (reached >= want) break;
      channel::BitcoinPeer peer;
      auto opened = peer.connect(seed, p.port, p.magic, 8000);
      if (!opened.ok()) {
        c.o << "  " << W{34} << seed << ": " << c.o.style(Red()) << opened.error().str() << c.o.style(Reset()) << "\n";
        continue;
      }
      auto shook = peer.handshake();
      if (!shook.ok()) {
        c.o << "  " << W{34} << seed << ": " << c.o.style(Red()) << shook.error().str() << c.o.style(Reset()) << "\n";
        continue;
      }
      ++reached;
      c.o << "  " << W{34} << peer.info().address << ": " << c.o.style(Green()) << "handshake" << c.o.style(Reset())
          << " protocol " << peer.info().version << " " << c.o.style(Cyan()) << peer.info().subversion
          << c.o.style(Reset()) << " height " << c.o.style(Yellow()) << peer.info().start_height
          << c.o.style(Reset()) << "\n";
      peer.close();
    }
    if (reached == 0) fail(Status::IoError, "no peer completed a handshake");
    c.o << "  " << reached << " peer(s) reached over the wire protocol, no gateway involved\n";
    return;
  }

  if (sub == "headers") {
    uint64_t batches = has_kv(a, "batches") ? to_u64(kv(a, "batches")) : 1;
    if (batches == 0) batches = 1;
    auto seeds = channel::dns_seeds(p.magic);

    channel::BitcoinPeer peer;
    channel::PeerInfo info;
    bool live = false;
    for (const auto& seed : seeds) {
      if (!peer.connect(seed, p.port, p.magic, 8000).ok()) continue;
      auto shook = peer.handshake();
      if (!shook.ok()) continue;
      info = *shook;
      live = true;
      break;
    }
    if (!live) fail(Status::IoError, "no peer completed a handshake");

    c.o << c.o.style(Header()) << "Bitcoin " << p.name << " headers from " << info.address << c.o.style(Reset()) << "\n";
    c.o << "  " << W{22} << "Peer" << ": " << info.subversion << " at height " << info.start_height << "\n";

    std::array<uint8_t, 32> cursor = chain_genesis(net);
    uint64_t total = 0, verified = 0, linked = 0;
    std::array<uint8_t, 32> previous = cursor;

    uint64_t reconnects = 0;
    for (uint64_t b = 0; b < batches; ++b) {
      auto headers = peer.get_headers({cursor});
      // A peer is free to drop us mid-stream and several do. The walk is
      // resumable from the cursor, so losing one costs a reconnect, not the run.
      for (size_t attempt = 0; !headers.ok() && attempt < seeds.size(); ++attempt) {
        peer.close();
        if (!peer.connect(seeds[attempt], p.port, p.magic, 8000).ok()) continue;
        if (!peer.handshake().ok()) continue;
        ++reconnects;
        headers = peer.get_headers({cursor});
      }
      if (!headers.ok()) fail(headers.status(), headers.error().message);
      if (headers->empty()) break;
      for (const auto& h : *headers) {
        ++total;
        if (h.work_is_valid()) ++verified;
        if (h.prev_block == previous) ++linked;
        previous = h.hash();
      }
      cursor = previous;
    }

    c.o << "  " << W{22} << "Headers received" << ": " << total << "\n";
    c.o << "  " << W{22} << "Proof of work valid" << ": " << c.o.style(verified == total ? Green() : Red())
        << verified << "/" << total << c.o.style(Reset()) << "\n";
    c.o << "  " << W{22} << "Chain links" << ": " << c.o.style(linked == total ? Green() : Red()) << linked << "/"
        << total << c.o.style(Reset()) << "\n";
    c.o << "  " << W{22} << "Tip of this walk" << ": " << chain::hash_to_display(cursor) << "\n";
    if (reconnects) c.o << "  " << W{22} << "Peers lost mid-walk" << ": " << reconnects << "\n";
    if (verified != total || linked != total) fail(Status::Corrupt, "peer served a chain that does not verify");
    peer.close();
    return;
  }

  if (sub == "trace") {
    auto seeds = channel::dns_seeds(p.magic);
    channel::BitcoinPeer peer;
    bool live = false;
    for (const auto& seed : seeds) {
      if (!peer.connect(seed, p.port, p.magic, 8000).ok()) continue;
      if (!peer.handshake().ok()) continue;
      live = true;
      break;
    }
    if (!live) fail(Status::IoError, "no peer completed a handshake");
    c.o << "traced peer " << peer.info().address << " " << peer.info().subversion << "\n";

    auto genesis = chain_genesis(net);
    ByteBuffer request;
    request.push_back(0x60); request.push_back(0x11); request.push_back(0x01); request.push_back(0x00);
    request.push_back(0x01);
    request.insert(request.end(), genesis.begin(), genesis.end());
    for (int i = 0; i < 32; ++i) request.push_back(0);
    need(peer.send("getheaders", ByteSpan(request.data(), request.size())));

    for (int i = 0; i < 12; ++i) {
      auto m = peer.receive(15000);
      if (!m.ok()) { c.o << "  recv failed: " << m.error().str() << "\n"; break; }
      c.o << "  <- " << W{14} << m->command << " " << m->payload.size() << " bytes\n";
      if (m->command == "headers") break;
      if (m->command == "ping") need(peer.send("pong", ByteSpan(m->payload.data(), m->payload.size())));
    }
    return;
  }

  if (sub == "rpc") {
    if (a.size() < 4) fail(Status::Invalid, "chain rpc <url> <method> [params...]");
    channel::ChainRpc rpc;
    need(rpc.open(a[2], 20000, &c.k.host(), c.k.config().root));
    std::vector<channel::RpcValue> params;
    for (size_t i = 4; i < a.size(); ++i) {
      auto parsed = channel::rpc_parse(a[i]);
      params.push_back(parsed.ok() ? *parsed : channel::RpcValue::text(a[i]));
    }
    auto out = rpc.call(a[3], params);
    if (!out.ok()) fail(out.status(), out.error().message);
    c.o << out->render() << "\n";
    return;
  }

  if (sub == "mine" || sub == "hashrate") {
    // Measures what this machine actually does against the target the network
    // actually enforces. Both numbers are taken, not assumed.
    uint64_t seconds = has_kv(a, "seconds") ? to_u64(kv(a, "seconds")) : 5;
    uint64_t threads = has_kv(a, "threads") ? to_u64(kv(a, "threads")) : 0;
    if (threads == 0) threads = c.k.host().topology().logical_cpus;
    if (threads == 0) threads = 4;

    channel::BlockHeader tip;
    uint64_t height = 0;
    std::string peer_name;
    if (has_kv(a, "bits")) {
      tip.bits = (uint32_t)std::strtoul(kv(a, "bits").c_str(), nullptr, 16);
      peer_name = "bits given on the command line";
    } else {
      auto seeds = channel::dns_seeds(p.magic);
      channel::BitcoinPeer peer;
      bool live = false;
      for (const auto& seed : seeds) {
        if (!peer.connect(seed, p.port, p.magic, 8000).ok()) continue;
        if (!peer.handshake().ok()) continue;
        live = true;
        break;
      }
      if (!live) fail(Status::IoError, "no peer completed a handshake");
      peer_name = peer.info().address + " " + peer.info().subversion;

      c.o << "walking headers to the tip (every one checked for work and linkage)\n";
      std::array<uint8_t, 32> cursor = chain_genesis(net);
      std::array<uint8_t, 32> previous = cursor;
      uint64_t bad = 0;
      while (true) {
        auto batch = peer.get_headers({cursor});
        if (!batch.ok()) {
          peer.close();
          bool again = false;
          for (const auto& seed : channel::dns_seeds(p.magic)) {
            if (!peer.connect(seed, p.port, p.magic, 8000).ok()) continue;
            if (!peer.handshake().ok()) continue;
            again = true;
            break;
          }
          if (!again) fail(Status::IoError, "lost every peer mid-walk");
          continue;
        }
        if (batch->empty()) break;
        for (const auto& h : *batch) {
          if (!h.work_is_valid() || h.prev_block != previous) ++bad;
          previous = h.hash();
          tip = h;
          ++height;
        }
        cursor = previous;
      }
      if (bad) fail(Status::Corrupt, "peer served " + std::to_string(bad) + " headers that do not verify");
      c.o << "  tip " << chain::hash_to_display(tip.hash()) << " at height " << height << "\n";
      peer.close();
    }

    double difficulty = channel::difficulty_of(tip.bits);
    c.o << c.o.style(Header()) << "What this machine can do against that target" << c.o.style(Reset()) << "\n";
    c.o << "  " << W{24} << "Source" << ": " << peer_name << "\n";
    c.o << "  " << W{24} << "Network nBits" << ": " << crypto::bytes_to_hex(ByteSpan(reinterpret_cast<const uint8_t*>(&tip.bits), 4)) << " (little endian)\n";
    c.o << "  " << W{24} << "Network difficulty" << ": " << fmt_double(difficulty, 0, true, 0) << "\n";

    auto measured = channel::measure_hashrate(tip, (uint32_t)threads, (uint32_t)seconds);
    c.o << "  " << W{24} << "Threads" << ": " << threads << "\n";
    c.o << "  " << W{24} << "Hashes done" << ": " << measured.hashes << " in " << seconds << "s\n";
    c.o << "  " << W{24} << "Measured rate" << ": " << c.o.style(Yellow())
        << fmt_double(measured.hashes_per_second / 1e6, 2, true, 2) << " MH/s" << c.o.style(Reset()) << "\n";
    c.o << "  " << W{24} << "Best hash seen" << ": " << measured.best_hash << "\n";

    // A block needs difficulty * 2^32 hashes on average. That is the whole
    // arithmetic; there is no term in it for cleverness.
    double expected_hashes = difficulty * 4294967296.0;
    double seconds_per_block = expected_hashes / (measured.hashes_per_second > 0 ? measured.hashes_per_second : 1.0);
    double years = seconds_per_block / (365.25 * 24 * 3600);

    // The figure is around 5e23, past what the fixed-point formatter can show,
    // so it is printed as a power of ten instead of as "inf".
    double exponent = 0.0;
    double mantissa = expected_hashes;
    while (mantissa >= 10.0) { mantissa /= 10.0; exponent += 1.0; }
    c.o << "  " << W{24} << "Hashes per block" << ": " << fmt_double(mantissa, 2, true, 2) << " x 10^"
        << (uint64_t)exponent << " expected\n";
    c.o << "  " << W{24} << "Time to one block" << ": " << c.o.style(Red()) << fmt_double(years, 0, true, 0)
        << " years" << c.o.style(Reset()) << " at this rate\n";
    c.o << "  " << W{24} << "Network finds one in" << ": 600 seconds\n";
    c.o << "  " << W{24} << "This machine's share" << ": "
        << fmt_double(measured.hashes_per_second / (expected_hashes / 600.0) * 100.0, 12, true, 12) << " %\n";
    return;
  }

  if (sub == "submit") {
    // The REST broadcast form: a raw transaction as the whole request body.
    // What comes back is a node's own verdict on bytes we serialized.
    need(c.me.operate());
    if (a.size() < 4) fail(Status::Invalid, "chain submit <url> <raw transaction hex>");
    auto endpoint = need(channel::parse_endpoint(a[2]));
    channel::HttpRequest request;
    request.method = "POST";
    request.endpoint = endpoint;
    request.timeout_ms = 30000;
    request.headers.emplace_back("Content-Type", "text/plain");
    request.body.assign(a[3].begin(), a[3].end());
    // https rides the machine's TLS client: it needs the host for randomness
    // and the trust store.
    request.host = &c.k.host();
    request.kernel_root = c.k.config().root;

    auto response = need(channel::http_send(request));
    c.o << "  node answered " << (response.status == 200 ? c.o.style(Green()) : c.o.style(Yellow()))
        << response.status << c.o.style(Reset()) << ": " << response.text() << "\n";
    return;
  }

  if (sub == "balance" || sub == "payout") {
    // chain balance [eth=<rpc-url>] [btc=<explorer-base-url>]
    userland::BitcoinWallet wallet(c.k);
    wallet.init("default", wallet.network());

    c.o << c.o.style(Header()) << "Payout accounts" << c.o.style(Reset()) << "  "
        << c.o.style(Yellow()) << "receive-only: AIOS holds no key for either" << c.o.style(Reset()) << "\n";

    c.o << "  " << W{10} << "ETH" << ": " << c.o.style(Cyan()) << wallet.eth_payout() << c.o.style(Reset()) << "\n";
    if (has_kv(a, "eth")) {
      channel::ChainRpc rpc;
      auto opened = rpc.open(kv(a, "eth"), 20000, &c.k.host(), c.k.config().root);
      if (!opened.ok()) {
        c.o << "  " << W{10} << "" << "  " << c.o.style(Red()) << opened.error().str() << c.o.style(Reset()) << "\n";
      } else {
        auto wei = rpc.call("eth_getBalance",
                            {channel::RpcValue::text(wallet.eth_payout()), channel::RpcValue::text("latest")});
        if (!wei.ok()) {
          c.o << "  " << W{10} << "" << "  " << c.o.style(Red()) << wei.error().message << c.o.style(Reset()) << "\n";
        } else {
          auto bytes = wei->as_quantity();
          // Wei does not fit a uint64 at scale, so the exact value stays as the
          // node's own digits and only the display figure is narrowed.
          double ether = 0.0;
          for (uint8_t b : bytes) ether = ether * 256.0 + (double)b;
          ether /= 1e18;
          c.o << "  " << W{10} << "" << "  balance " << c.o.style(Yellow()) << fmt_double(ether, 8, true, 8)
              << " ETH" << c.o.style(Reset()) << "  (" << wei->as_text() << " wei)\n";
          auto nonce = rpc.call("eth_getTransactionCount",
                                {channel::RpcValue::text(wallet.eth_payout()), channel::RpcValue::text("latest")});
          if (nonce.ok()) c.o << "  " << W{10} << "" << "  " << nonce->as_u64() << " transactions sent from it\n";
        }
      }
    }

    c.o << "  " << W{10} << "BTC" << ": " << c.o.style(Cyan()) << wallet.watched() << c.o.style(Reset()) << "\n";
    if (has_kv(a, "btc")) {
      std::string url = kv(a, "btc") + "/api/address/" + wallet.watched();
      auto page = channel::http_get_secure(url, c.k.host(), c.k.config().root, 25000);
      if (!page.ok()) {
        c.o << "  " << W{10} << "" << "  " << c.o.style(Red()) << page.error().str() << c.o.style(Reset()) << "\n";
      } else if (page->status != 200) {
        c.o << "  " << W{10} << "" << "  " << c.o.style(Red()) << "source answered " << page->status
            << c.o.style(Reset()) << "\n";
      } else {
        auto doc = channel::rpc_parse(ByteSpan(page->body.data(), page->body.size()));
        if (!doc.ok()) {
          c.o << "  " << W{10} << "" << "  " << c.o.style(Red()) << doc.error().str() << c.o.style(Reset()) << "\n";
        } else {
          const auto& confirmed = (*doc)["chain_stats"];
          const auto& pending = (*doc)["mempool_stats"];
          uint64_t funded = confirmed["funded_txo_sum"].as_u64();
          uint64_t spent = confirmed["spent_txo_sum"].as_u64();
          uint64_t sats = funded >= spent ? funded - spent : 0;
          c.o << "  " << W{10} << "" << "  balance " << c.o.style(Yellow())
              << fmt_double((double)sats / 100000000.0, 8, true, 8) << " BTC" << c.o.style(Reset()) << "  ("
              << sats << " sats)\n";
          c.o << "  " << W{10} << "" << "  " << confirmed["tx_count"].as_u64() << " confirmed transactions, "
              << pending["tx_count"].as_u64() << " in the mempool\n";
          // That figure came from an index we did not build. Anchoring it to a
          // tip height taken straight off the wire at least says which chain the
          // answer is about, and that this machine agrees such a chain exists.
          for (const auto& seed : channel::dns_seeds(chain::network_params(chain::Network::Mainnet).magic)) {
            channel::BitcoinPeer peer;
            if (!peer.connect(seed, 8333, chain::network_params(chain::Network::Mainnet).magic, 6000).ok()) continue;
            auto shook = peer.handshake();
            if (!shook.ok()) continue;
            c.o << "  " << W{10} << "" << "  source: index at " << kv(a, "btc") << ", against a chain whose tip\n";
            c.o << "  " << W{10} << "" << "          peer " << shook->address << " puts at height "
                << c.o.style(Yellow()) << shook->start_height << c.o.style(Reset()) << "\n";
            peer.close();
            break;
          }
        }
      }
    }

    if (!has_kv(a, "eth") && !has_kv(a, "btc")) {
      c.o << "  pass eth=<rpc-url> and btc=<explorer-base> to read live balances\n";
    }
    return;
  }

  if (sub == "eth") {
    // chain eth <rpc-url> to=<addr> [value=<wei>] [passphrase=<p>] [submit]
    if (a.size() < 3) fail(Status::Invalid, "chain eth <rpc-url> to=<addr> [value=<wei>] [submit]");
    userland::BitcoinWallet wallet(c.k);
    if (has_kv(a, "passphrase")) wallet.unlock(kv(a, "passphrase"));
    wallet.init("default", wallet.network());
    if (has_kv(a, "passphrase")) wallet.unlock(kv(a, "passphrase"));
    if (!wallet.unlock_error().empty()) fail(Status::Denied, wallet.unlock_error());
    if (!wallet.has_key()) fail(Status::NotFound, "no key held; 'wallet new passphrase=<p>' first");

    channel::ChainRpc rpc;
    need(rpc.open(a[2], 20000, &c.k.host(), c.k.config().root));

    auto chain_id = rpc.call("eth_chainId");
    if (!chain_id.ok()) fail(chain_id.status(), chain_id.error().message);
    std::string from = wallet.eth_account();
    auto nonce = rpc.call("eth_getTransactionCount",
                          {channel::RpcValue::text(from), channel::RpcValue::text("pending")});
    if (!nonce.ok()) fail(nonce.status(), nonce.error().message);
    auto tip = rpc.call("eth_maxPriorityFeePerGas");
    auto gas_price = rpc.call("eth_gasPrice");

    chain::EthTransaction tx;
    tx.chain_id = chain_id->as_u64();
    tx.nonce = nonce->as_u64();
    tx.gas_limit = 21000;
    tx.to = need(chain::eth_parse_address(kv(a, "to", "0x0000000000000000000000000000000000000000")));
    tx.value = chain::eth_quantity_from_hex(kv(a, "value", "0x2386f26fc10000"));
    tx.max_priority_fee_per_gas =
        tip.ok() ? chain::eth_quantity_from_hex(tip->as_text()) : chain::eth_quantity_from_hex("0x3b9aca00");
    tx.max_fee_per_gas =
        gas_price.ok() ? chain::eth_quantity_from_hex(gas_price->as_text()) : chain::eth_quantity_from_hex("0x2540be400");

    c.o << c.o.style(Header()) << "Ethereum transaction on chain " << tx.chain_id << c.o.style(Reset()) << "\n";
    c.o << "  " << W{18} << "from" << ": " << from << "\n";
    c.o << "  " << W{18} << "nonce" << ": " << tx.nonce << " (from the node)\n";
    c.o << "  " << W{18} << "max fee" << ": " << chain::eth_hex_of(ByteSpan(tx.max_fee_per_gas.data(), tx.max_fee_per_gas.size())) << " wei/gas\n";

    // What the node will ask for, worked out before asking it, so the answer to
    // "do I have enough yet" does not have to be read out of a rejection.
    auto wei_of = [](ByteSpan b) {
      double v = 0.0;
      for (uint8_t byte : b) v = v * 256.0 + (double)byte;
      return v;
    };
    double fee_ceiling = wei_of(ByteSpan(tx.max_fee_per_gas.data(), tx.max_fee_per_gas.size()));
    double sending = wei_of(ByteSpan(tx.value.data(), tx.value.size()));
    double gas_cost = (double)tx.gas_limit * fee_ceiling;
    double needed = sending + gas_cost;

    auto held = rpc.call("eth_getBalance", {channel::RpcValue::text(from), channel::RpcValue::text("latest")});
    double balance = 0.0;
    if (held.ok()) balance = wei_of(ByteSpan(held->as_quantity().data(), held->as_quantity().size()));

    c.o << "  " << W{18} << "gas at that price" << ": " << fmt_double(gas_cost / 1e18, 8, true, 8) << " ETH ("
        << tx.gas_limit << " gas)\n";
    c.o << "  " << W{18} << "sending" << ": " << fmt_double(sending / 1e18, 8, true, 8) << " ETH\n";
    c.o << "  " << W{18} << "account needs" << ": " << fmt_double(needed / 1e18, 8, true, 8) << " ETH\n";
    c.o << "  " << W{18} << "account holds" << ": " << fmt_double(balance / 1e18, 8, true, 8) << " ETH\n";
    if (balance >= needed) {
      c.o << "  " << W{18} << "verdict" << ": " << c.o.style(Green()) << "funded, this will go through"
          << c.o.style(Reset()) << "\n";
    } else {
      c.o << "  " << W{18} << "verdict" << ": " << c.o.style(Yellow()) << "short by "
          << fmt_double((needed - balance) / 1e18, 8, true, 8) << " ETH" << c.o.style(Reset())
          << " — the node will refuse until the balance covers it\n";
    }

    auto raw = need(wallet.sign_ethereum(tx));
    std::string raw_hex = crypto::bytes_to_hex(ByteSpan(raw.data(), raw.size()));
    auto recovered = need(chain::eth_recover_sender(ByteSpan(raw.data(), raw.size())));

    c.o << "  " << W{18} << "signed sender" << ": " << chain::eth_checksum_address(recovered) << "\n";
    c.o << "  " << W{18} << "tx hash" << ": 0x"
        << crypto::bytes_to_hex(ByteSpan(chain::eth_transaction_hash(ByteSpan(raw.data(), raw.size())).data(), 32))
        << "\n";
    c.o << "  " << W{18} << "raw" << ": " << raw_hex << "\n";

    bool submit = false;
    for (size_t i = 3; i < a.size(); ++i) {
      if (a[i] == "submit") submit = true;
    }
    if (!submit) {
      c.o << "  not submitted; add 'submit' to hand it to the node\n";
      return;
    }

    need(c.me.operate());
    auto sent = rpc.call("eth_sendRawTransaction", {channel::RpcValue::text("0x" + raw_hex)});
    if (sent.ok()) {
      c.o << "  " << c.o.style(Green()) << "accepted" << c.o.style(Reset()) << ": " << sent->as_text() << "\n";
    } else {
      // What the node objects to is the point: a complaint about funds means it
      // parsed the envelope and recovered the sender before it got there.
      c.o << "  " << c.o.style(Yellow()) << "node refused" << c.o.style(Reset()) << ": "
          << sent.error().message << "\n";
    }
    return;
  }

  if (sub == "decode") {
    if (a.size() < 3) fail(Status::Invalid, "chain decode <raw transaction hex>");
    auto raw = crypto::hex_to_bytes(a[2]);
    auto tx = chain::deserialize_tx(ByteSpan(raw.data(), raw.size()));
    if (!tx.ok()) fail(tx.status(), tx.error().message);
    c.o << c.o.style(Header()) << "Transaction" << c.o.style(Reset()) << "\n";
    c.o << "  " << W{18} << "txid" << ": " << chain::hash_to_display(chain::txid_of(*tx)) << "\n";
    c.o << "  " << W{18} << "wtxid" << ": " << chain::hash_to_display(chain::wtxid_of(*tx)) << "\n";
    c.o << "  " << W{18} << "version" << ": " << tx->version << "\n";
    c.o << "  " << W{18} << "vsize" << ": " << chain::vsize_of(*tx) << " vB\n";
    c.o << "  " << W{18} << "inputs" << ": " << tx->inputs.size() << "\n";
    for (const auto& in : tx->inputs) {
      c.o << "    " << chain::hash_to_display(in.prevout.txid) << ":" << in.prevout.index
          << (in.witness.empty() ? "" : "  (witness)") << "\n";
    }
    c.o << "  " << W{18} << "outputs" << ": " << tx->outputs.size() << "\n";
    for (const auto& o : tx->outputs) {
      auto addr = chain::address_for_script(ByteSpan(o.script_pubkey.data(), o.script_pubkey.size()), net);
      c.o << "    " << W{16} << o.value << " sats  " << (addr.ok() ? *addr : std::string("(no address form)")) << "\n";
    }
    return;
  }

  if (sub == "broadcast" || sub == "send") {
    // Handing a transaction to the network is the one act here that moves value
    // and cannot be taken back, so it goes through the kernel's authority rather
    // than round it.
    need(c.me.operate());
    if (a.size() < 3) fail(Status::Invalid, "chain broadcast <raw transaction hex> [network]");
    auto raw = crypto::hex_to_bytes(a[2]);
    auto tx = chain::deserialize_tx(ByteSpan(raw.data(), raw.size()));
    if (!tx.ok()) fail(tx.status(), tx.error().message);
    auto txid = chain::txid_of(*tx);
    auto wtxid = chain::wtxid_of(*tx);

    c.o << c.o.style(Header()) << "Broadcasting on " << p.name << c.o.style(Reset()) << "\n";
    c.o << "  " << W{18} << "txid" << ": " << chain::hash_to_display(txid) << "\n";

    uint64_t handed = 0;
    for (const auto& seed : channel::dns_seeds(p.magic)) {
      channel::BitcoinPeer peer;
      auto opened = peer.connect(seed, p.port, p.magic, 8000);
      if (!opened.ok()) {
        c.o << "  " << W{34} << seed << ": " << opened.error().str() << "\n";
        continue;
      }
      auto shook = peer.handshake();
      if (!shook.ok()) {
        c.o << "  " << W{34} << peer.info().address << ": " << shook.error().str() << "\n";
        continue;
      }
      auto offered = peer.offer_transaction(ByteSpan(raw.data(), raw.size()), txid, wtxid, 20000);
      if (!offered.ok()) {
        c.o << "  " << W{34} << peer.info().address << ": " << c.o.style(Red()) << offered.error().str()
            << c.o.style(Reset()) << "\n";
        continue;
      }
      c.o << "  " << W{34} << peer.info().address << ": " << c.o.style(*offered ? Green() : Yellow())
          << (*offered ? "requested and handed over" : "announced, not requested") << c.o.style(Reset()) << "\n";
      if (*offered) ++handed;
      peer.close();
      if (handed >= 3) break;
    }
    c.o << "  " << W{18} << "peers that took it" << ": " << handed << "\n";
    if (handed == 0) fail(Status::IoError, "no peer asked for the transaction body");
    return;
  }

  fail(Status::Invalid,
       "chain peers | headers | balance | decode <hex> | broadcast <hex> | eth <url> | rpc <url> <method>");
}

void cmd_wallet(Ctx& c, const Args& a) {
  c.view();
  userland::BitcoinWallet wallet(c.k);
  // The subcommand is the first word that is not a key=value option, so
  // 'wallet passphrase=<p>' is 'wallet status' with a passphrase.
  std::string sub;
  std::vector<std::string> pos;   // positional words after the subcommand
  for (size_t i = 1; i < a.size(); ++i) {
    if (a[i].find('=') != std::string::npos) continue;
    if (sub.empty()) sub = a[i]; else pos.push_back(a[i]);
  }
  if (sub.empty()) sub = "status";
  auto arg = [&](size_t n) -> const std::string& {
    static const std::string none;
    return n < pos.size() ? pos[n] : none;
  };
  if (has_kv(a, "passphrase")) wallet.unlock(kv(a, "passphrase"));
  wallet.init("default");
  const std::string file_network = chain::network_params(wallet.network()).name;
  if (has_kv(a, "network")) {
    auto n = chain::network_by_name(kv(a, "network"));
    if (!n.ok()) fail(Status::Invalid, "unknown network '" + kv(a, "network") + "' (mainnet, testnet, signet, regtest)");
    if (*n != wallet.network()) {
      wallet.set_network(*n);
      c.o << "network " << file_network << " -> " << chain::network_params(*n).name;
      if (sub != "new" && sub != "generate" && sub != "import") {
        need(wallet.save_state());
        c.o << " (saved)";
      }
      c.o << "\n";
    }
  }
  if (has_kv(a, "passphrase") && !wallet.unlock_error().empty()) {
    // A wrong passphrase is an error, never a silent "locked" or, worse, a
    // status line that claims it encrypted something.
    fail(Status::Denied, wallet.unlock_error());
  }

  if (sub == "status" || sub == "info" || sub == "balance") {
    c.o << c.o.style(Header()) << "Wallet on " << chain::network_params(wallet.network()).name
        << c.o.style(Reset()) << "\n";
    if (!wallet.has_key()) {
      if (wallet.sealed_on_file()) {
        c.o << "  " << c.o.style(Yellow()) << "key sealed on file, not opened" << c.o.style(Reset())
            << " — pass passphrase=<p> to use it\n";
      } else {
        c.o << "  " << c.o.style(Yellow()) << "no key held" << c.o.style(Reset())
            << " — 'wallet new passphrase=<p>' to create one, or 'wallet import <wif> passphrase=<p>'\n";
      }
      if (!wallet.address().empty()) c.o << "  " << W{20} << "Address on file" << ": " << wallet.address() << "\n";
    } else {
      c.o << "  " << W{20} << "Address" << ": " << c.o.style(Cyan()) << wallet.address() << c.o.style(Reset()) << "\n";
      c.o << "  " << W{20} << "Legacy address" << ": " << wallet.legacy_address() << "\n";
      c.o << "  " << W{20} << "Ethereum account" << ": " << c.o.style(Cyan()) << wallet.eth_account() << c.o.style(Reset()) << "\n";
      c.o << "  " << W{20} << "Public key" << ": " << wallet.pubkey_hex() << "\n";
    }
    if (!wallet.node_url().empty()) {
      auto synced = wallet.sync_from_node();
      if (!synced.ok()) {
        c.o << "  " << W{20} << "Node" << ": " << c.o.style(Red()) << synced.error().str() << c.o.style(Reset()) << "\n";
      }
    }
    c.o << "  " << W{20} << "Node" << ": "
        << (wallet.node_url().empty() ? std::string("none set — 'wallet node <url>'") : wallet.node_url()) << "\n";
    c.o << "  " << W{20} << "Spendable" << ": " << c.o.style(Yellow())
        << fmt_double(wallet.balance_btc(), 8, true, 8) << c.o.style(Reset()) << " ("
        << wallet.balance_sats() << " sats, " << wallet.utxos().size() << " outputs on record)\n";
    if (wallet.reserved_sats() > 0)
      c.o << "  " << W{20} << "Reserved" << ": " << wallet.reserved_sats()
          << " sats held by transactions signed here ('wallet history')\n";
    if (!wallet.watched().empty()) {
      c.o << "  " << W{20} << "Watching" << ": " << wallet.watched() << " ("
          << wallet.watched_balance_sats() << " sats, " << c.o.style(Yellow()) << "not spendable: no key"
          << c.o.style(Reset()) << ")\n";
    }
    std::string at_rest;
    if (wallet.sealed_on_file())
      at_rest = std::string("sealed on file (ChaCha20-Poly1305, ") + wallet.kdf_description() + ")" +
                (wallet.has_key() ? ", opened with the given passphrase" : "");
    else if (wallet.has_key())
      at_rest = "not written — no passphrase supplied";
    else
      at_rest = "none";
    c.o << "  " << W{20} << "Key at rest" << ": " << at_rest << "\n";
    c.o << "  " << W{20} << "History" << ": " << wallet.tx_history().size() << " transactions\n";
  } else if (sub == "new" || sub == "generate" || sub == "import") {
    // Replacing a key loses it for good if it is the only copy, so it takes
    // force=1 to do.
    if (wallet.key_on_file() && kv(a, "force") != "1") {
      fail(Status::Conflict, std::string("a key is already on file for ") + wallet.address() +
                               "; 'wallet " + sub + "' would replace it. Back it up ('wallet export passphrase=<p>') "
                               "and pass force=1 to replace it.");
    }
    if (!has_kv(a, "passphrase") || kv(a, "passphrase").empty()) {
      c.o << c.o.style(Yellow()) << "no passphrase given" << c.o.style(Reset())
          << " — the key will exist for this command only. Pass passphrase=<p> to persist it.\n";
    }
    if (sub == "import") {
      if (arg(0).empty()) fail(Status::Invalid, "wallet import <wif> [passphrase=<p>] [force=1]");
      need(wallet.import_wif(arg(0)));
      c.o << "imported. address " << wallet.address() << "\n";
    } else {
      need(wallet.generate_new_keys());
      c.o << c.o.style(Green()) << "key created" << c.o.style(Reset()) << " on "
          << chain::network_params(wallet.network()).name << "\n";
      c.o << "  " << W{20} << "Address" << ": " << wallet.address() << "\n";
      c.o << "  " << W{20} << "Ethereum account" << ": " << wallet.eth_account() << "\n";
    }
    if (wallet.sealed_on_file())
      c.o << "  " << W{20} << "Key at rest" << ": sealed (" << wallet.kdf_description() << ")\n";
  } else if (sub == "export" || sub == "wif") {
    if (!wallet.has_key()) fail(Status::NotFound, wallet.sealed_on_file()
                                                      ? "the key is sealed on file; pass passphrase=<p> to export it"
                                                      : "no key held; nothing to export");
    c.o << "Address     : " << wallet.address() << "\n";
    c.o << "Ethereum    : " << wallet.eth_account() << "\n";
    c.o << "WIF         : " << wallet.export_wif() << "\n";
  } else if (sub == "node") {
    if (!arg(0).empty()) {
      wallet.set_node(arg(0));
      need(wallet.save_state());
      c.o << "node set to " << wallet.node_url() << "\n";
    } else {
      c.o << "node: " << (wallet.node_url().empty() ? "none" : wallet.node_url()) << "\n";
    }
  } else if (sub == "watch") {
    if (arg(0).empty()) fail(Status::Invalid, "wallet watch <address>");
    // Only something that decodes to an output script on this network is an
    // address a node can be asked about.
    if (!chain::script_for_address(arg(0), wallet.network()).ok())
      fail(Status::Invalid, "'" + arg(0) + "' is not a " + chain::network_params(wallet.network()).name +
                                " address (pass network=<net> if it belongs to another network)");
    wallet.watch(arg(0));
    need(wallet.save_state());
    c.o << "watching " << wallet.watched() << c.o.style(Yellow()) << " (no key held for it; it cannot be spent)"
        << c.o.style(Reset()) << "\n";
  } else if (sub == "utxo") {
    if (pos.size() < 4) fail(Status::Invalid, "wallet utxo <txid> <vout> <sats> <scriptpubkey-hex>");
    userland::UTXO u;
    u.txid = arg(0);
    u.vout = (uint32_t)to_u64(arg(1));
    u.amount_sats = to_u64(arg(2));
    u.script_pubkey = arg(3);
    wallet.add_utxo(u);
    c.o << "recorded " << u.amount_sats << " sats at " << u.txid << ":" << u.vout << "\n";
  } else if (sub == "sync") {
    need(wallet.sync_from_node());
    c.o << "synced from " << wallet.live_sync_source() << "\n";
    c.o << "  " << wallet.balance_sats() << " sats spendable across " << wallet.utxos().size() << " outputs";
    if (wallet.reserved_sats()) c.o << " (" << wallet.reserved_sats() << " sats reserved by signed transactions)";
    c.o << "\n";
  } else if (sub == "send" || sub == "transfer") {
    need(c.me.operate());
    if (pos.size() < 2) fail(Status::Invalid, "wallet send <recipient> <amount_sats> [feerate=<sat/vB>] passphrase=<p>");
    if (!wallet.has_key())
      fail(Status::NotFound, wallet.sealed_on_file() ? "the key is sealed on file; pass passphrase=<p> to sign"
                                                     : "no key held; 'wallet new passphrase=<p>' first");
    uint64_t rate = has_kv(a, "feerate") ? to_u64(kv(a, "feerate")) : 2;
    auto tx = need(wallet.create_and_sign_transaction(arg(0), to_u64(arg(1)), rate));
    // Recorded before it is shown: the outputs it spends are reserved, so a
    // second send cannot sign the same coins.
    need(wallet.record(tx));
    c.o << c.o.style(Green()) << "signed" << c.o.style(Reset()) << ", not broadcast\n";
    c.o << "  " << W{20} << "txid" << ": " << tx.txid << "\n";
    c.o << "  " << W{20} << "size" << ": " << tx.vsize << " vB, fee " << tx.fee_sats << " sats\n";
    c.o << "  " << W{20} << "raw" << ": " << tx.raw_hex << "\n";
    c.o << "  " << W{20} << "recorded" << ": in history; its inputs are reserved ('wallet release " << tx.txid
        << "' if it is never sent)\n";
    c.o << "  broadcast it with: chain broadcast <raw> " << chain::network_params(wallet.network()).name << "\n";
  } else if (sub == "history") {
    c.o << c.o.style(Header()) << "Transactions signed by this wallet" << c.o.style(Reset()) << "\n";
    if (wallet.tx_history().empty()) c.o << "  none\n";
    for (const auto& tx : wallet.tx_history()) {
      size_t held = 0;
      for (const auto& u : wallet.utxos()) if (u.reserved_by == tx.txid) ++held;
      c.o << "  " << tx.txid << "  " << tx.amount_sats << " sats -> " << tx.recipient << "  fee " << tx.fee_sats
          << "  " << tx.vsize << " vB  " << (held ? std::to_string(held) + " input(s) reserved" : std::string("inputs no longer on record"))
          << "\n";
    }
  } else if (sub == "release") {
    if (arg(0).empty()) fail(Status::Invalid, "wallet release <txid>");
    size_t freed = wallet.release(arg(0));
    if (!freed) fail(Status::NotFound, "no output is reserved by " + arg(0));
    c.o << "released " << freed << " output(s) reserved by " << arg(0) << "\n";
  } else {
    fail(Status::Invalid,
         "wallet status | new [force=1] | import <wif> [force=1] | export | node [url] | watch <addr> | sync | "
         "send <addr> <sats> | history | release <txid>   [passphrase=<p>] [network=<net>]");
  }
}

void cmd_contract(Ctx& c, const Args& a) {
  c.view();
  userland::BitcoinWallet wallet(c.k);
  wallet.init("default");
  if (has_kv(a, "network")) {
    auto n = chain::network_by_name(kv(a, "network"));
    if (!n.ok()) fail(Status::Invalid, "unknown network '" + kv(a, "network") + "'");
    wallet.set_network(*n);
  }
  userland::BitcoinContractSolver solver(c.k, wallet);
  const std::string net_name = chain::network_params(wallet.network()).name;

  std::string sub = (a.size() > 1 && a[1].find('=') == std::string::npos) ? a[1] : "solve";
  bool force_gpu = false;
  bool loop_indefinitely = false;
  uint32_t rounds = 1;
  for (size_t i = 1; i < a.size(); ++i) {
    if (a[i] == "gpu") force_gpu = true;
    if (a[i] == "watch" && i + 1 < a.size() && a[i + 1].find('=') == std::string::npos) {
      const std::string& arg = a[i + 1];
      if (arg == "loop" || arg == "infinite" || arg == "continuous" || arg == "0") loop_indefinitely = true;
      else rounds = (uint32_t)to_u64(arg);
    } else if (a[i] == "watch") {
      rounds = 3;
    }
  }
  if (has_kv(a, "rounds")) rounds = (uint32_t)to_u64(kv(a, "rounds"));
  uint32_t threads = has_kv(a, "threads") ? (uint32_t)to_u64(kv(a, "threads")) : 0;
  uint32_t space = has_kv(a, "space") ? (uint32_t)to_u64(kv(a, "space")) : 100000;
  if (space == 0) space = 1;

  if (sub == "solve" || sub == "run" || sub == "btc" || sub == "real" || sub == "watch") {
    c.o << c.o.style(Header()) << "Hash-locked contract puzzle (local — not on any chain)" << c.o.style(Reset()) << "\n";
    c.o << "  " << W{22} << "Network" << ": " << net_name << " (the wallet's)\n";
    std::string source;
    uint64_t height = 0;
    if (solver.chain_tip(source, height))
      c.o << "  " << W{22} << "Chain tip" << ": height " << height << ", as claimed by " << source << "\n";
    else
      c.o << "  " << W{22} << "Chain tip" << ": " << c.o.style(Yellow()) << "no " << net_name << " peer reachable"
          << c.o.style(Reset()) << "\n";
    c.o << "  " << W{22} << "Funded by" << ": nothing — no transaction pays into this script, so solving it pays nobody\n";
    c.o.flush();

    for (uint32_t round = 1; loop_indefinitely || round <= rounds; ++round) {
      uint32_t secret = 0;
      c.k.host().random(MutableByteSpan(reinterpret_cast<uint8_t*>(&secret), sizeof secret));
      secret %= space;
      auto spec = solver.create_solvable_puzzle(secret, space, (uint32_t)(height ? height + 144 : 0));
      if (rounds > 1 || loop_indefinitely) c.o << c.o.style(Header()) << "[round " << round << "]" << c.o.style(Reset()) << "\n";
      c.o << "  " << W{22} << "Hash lock" << ": sha256(preimage) = " << spec.target_hash_hex << "\n";
      if (!spec.script_hex.empty())
        c.o << "  " << W{22} << "HTLC script" << ": " << spec.script_hex << "\n";
      else
        c.o << "  " << W{22} << "HTLC script" << ": not built — the wallet has no public key on file ('wallet new')\n";
      auto sol = need(solver.solve_contract(spec, force_gpu, threads));
      double mhs = sol.solve_ns ? (double)sol.hashes_evaluated * 1e3 / (double)sol.solve_ns : 0.0;
      c.o << "  " << W{22} << "Search" << ": " << sol.hashes_evaluated << " of " << space << " candidates in "
          << fmt_ns(sol.solve_ns) << " (" << fmt_double(mhs, 3, true, 3) << " MH/s), " << sol.accelerator_used << "\n";
      if (!sol.solved) {
        c.o << "  " << c.o.style(Red()) << "not solved" << c.o.style(Reset()) << " in the search space\n";
        continue;
      }
      c.o << "  " << W{22} << "Preimage" << ": " << userland::BitcoinContractSolver::preimage_for(sol.winning_nonce)
          << " (nonce " << sol.winning_nonce << ")\n";
      // The preimage is checked by the script interpreter, not by comparing
      // strings: OP_SHA256 <h> OP_EQUAL must leave true.
      auto checked = solver.evaluate_script("a820" + spec.target_hash_hex + "87", {sol.preimage_hex});
      bool ok = checked.ok() && *checked;
      c.o << "  " << W{22} << "Script check" << ": " << c.o.style(ok ? Green() : Red())
          << (ok ? "OP_SHA256 <h> OP_EQUAL is true for it" : "OP_SHA256 <h> OP_EQUAL failed")
          << c.o.style(Reset()) << " (the signature branch needs a spending transaction and was not run)\n";
      if (!ok) fail(Status::Corrupt, "a found preimage did not satisfy the hash lock");
      c.o.flush();
    }
    return;
  }
  if (sub == "create" || sub == "htlc") {
    // contract create hash=<64 hex> [lock=<height>]
    std::string h = kv(a, "hash");
    if (h.size() != 64) fail(Status::Invalid, "contract create hash=<sha256 hex, 64 chars> [lock=<height>]");
    std::string script = solver.htlc_script(h, (uint32_t)to_u64(kv(a, "lock", "0")));
    if (script.empty()) fail(Status::NotFound, "the wallet has no public key on file to lock to ('wallet new')");
    c.o << "HTLC script (" << net_name << ", not broadcast, nothing funds it): " << script << "\n";
    return;
  }
  fail(Status::Invalid,
       "contract solve [watch N] [threads=N] [space=N] [gpu] [network=<net>] | create hash=<hex> [lock=<height>]");
}

void cmd_task(Ctx& c, const Args& a) {
  c.view();
  userland::TaskLedger ledger(c.k);
  ledger.init("tasks");

  std::string sub = (a.size() > 1) ? a[1] : "list";
  if (sub == "create" || sub == "add" || sub == "new") {
    std::string cid = kv(a, "customer", "default");
    std::string name = kv(a, "name", "");
    std::string payload = kv(a, "payload", "");
    uint32_t priority = 1;
    std::string prio_str = kv(a, "priority", "1");
    if (!prio_str.empty()) priority = (uint32_t)to_u64(prio_str);

    if (name.empty()) {
      for (size_t i = 2; i < a.size(); ++i) {
        if (a[i].find('=') == std::string::npos) {
          name = a[i];
          break;
        }
      }
    }
    if (name.empty()) fail(Status::Invalid, "task create customer=<cid> name=<name> payload=<payload> [priority=<p>]");

    auto id_res = ledger.create_task(cid, name, payload, priority);
    std::string task_id = need(id_res);
    c.produced(task_id);

    c.o << c.o.style(Green()) << "[TASK CREATED]" << c.o.style(Reset()) << " ID: " << c.o.style(Cyan()) << task_id << c.o.style(Reset()) << " (Customer: " << cid << ", Name: '" << name << "')\n";
  } else if (sub == "list" || sub == "ls") {
    std::string cid = kv(a, "customer", "");
    std::string st = kv(a, "status", "");
    auto tasks = ledger.list_tasks(cid, st);

    c.o << c.o.style(Header()) << "Task Ledger Directory (" << tasks.size() << " task(s))" << c.o.style(Reset()) << "\n";
    c.o << "  " << W{18} << "Task ID" << "  " << W{14} << "Customer" << "  " << W{14} << "Status" << "  " << W{8} << "Prio" << "  " << "Task Name\n";
    c.o << "  " << std::string(75, '-') << "\n";
    for (auto& t : tasks) {
      c.o << "  " << W{18} << t.id << "  " << W{14} << t.customer_id << "  " << W{14} << userland::task_status_to_string(t.status) << "  " << W{8} << t.priority << "  " << t.name << "\n";
    }
  } else if (sub == "status" || sub == "update") {
    if (a.size() < 4) fail(Status::Invalid, "task status <task_id> <PENDING|IN_PROGRESS|COMPLETED|FAILED|CANCELLED> [details=<text>]");
    std::string task_id = c.r.expand(a[2]);
    auto target_st = need(userland::task_status_from_string(a[3]));

    std::string details = kv(a, "details", "");
    need(ledger.update_status(task_id, target_st, details));

    c.o << c.o.style(Green()) << "[SUCCESS]" << c.o.style(Reset()) << " Updated task " << task_id << " status to " << userland::task_status_to_string(target_st) << "\n";
  } else if (sub == "ledger" || sub == "log" || sub == "history") {
    std::string task_id = kv(a, "task_id", (a.size() > 2 && a[2].find('=') == std::string::npos) ? a[2] : "");
    std::string cid = kv(a, "customer", "");
    auto history = ledger.get_ledger_history(task_id, cid);

    bool ok = ledger.verify_ledger_integrity();
    c.o << c.o.style(Header()) << "Append-Only Task Ledger Log (" << history.size() << " entries, Cryptographic Integrity: " << (ok ? "VERIFIED VALID" : "INVALID") << ")" << c.o.style(Reset()) << "\n";
    c.o << "  " << W{6} << "Idx" << "  " << W{18} << "Task ID" << "  " << W{12} << "Action" << "  " << W{12} << "State Trans" << "  " << W{16} << "SHA256 Digest (Truncated)" << "  " << "Details\n";
    c.o << "  " << std::string(90, '-') << "\n";
    for (auto& e : history) {
      std::string trans = e.prev_status.empty() ? e.new_status : (e.prev_status + " -> " + e.new_status);
      c.o << "  " << W{6} << e.entry_index << "  " << W{18} << e.task_id << "  " << W{12} << e.action << "  " << W{12} << trans << "  " << W{16} << e.hash.substr(0, 16) << "...  " << e.details << "\n";
    }
  } else if (sub == "show" || sub == "get") {
    if (a.size() < 3) fail(Status::Invalid, "task show <task_id>");
    auto t = need(ledger.get_task(a[2]));

    c.o << c.o.style(Header()) << "Task Record: " << t.id << c.o.style(Reset()) << "\n";
    c.o << "  " << W{20} << "Customer ID" << ": " << t.customer_id << "\n";
    c.o << "  " << W{20} << "Name" << ": " << t.name << "\n";
    c.o << "  " << W{20} << "Payload" << ": " << (t.payload.empty() ? "(none)" : t.payload) << "\n";
    c.o << "  " << W{20} << "Status" << ": " << userland::task_status_to_string(t.status) << "\n";
    c.o << "  " << W{20} << "Priority" << ": " << t.priority << "\n";
    c.o << "  " << W{20} << "Created At (ns)" << ": " << t.created_at_ns << "\n";
    c.o << "  " << W{20} << "Updated At (ns)" << ": " << t.updated_at_ns << "\n";
  } else if (sub == "speech-add" || sub == "voice-add" || sub == "speech-update" || sub == "voice-update" ||
             sub == "speak" || sub == "tts" || sub == "speak-list" || sub == "tts-list") {
    // Every speech subcommand works on the ledger's own tasks: what it reads
    // aloud is what the ledger holds, and what it hears is written back to it.
    auto as_item = [](const userland::TaskRecord& t) {
      userland::TaskItem item;
      item.id = t.id;
      item.title = t.name;
      item.description = t.payload;
      item.status = t.status;
      item.priority = t.priority >= 3 ? userland::TaskPriority::Critical
                      : t.priority == 2 ? userland::TaskPriority::High
                      : t.priority == 1 ? userland::TaskPriority::Medium : userland::TaskPriority::Low;
      item.created_at_ns = t.created_at_ns;
      item.updated_at_ns = t.updated_at_ns;
      return item;
    };
    // The synthesizer is started only once the ledger has what is to be said:
    // a task that does not exist is reported as such, not as a missing voice.
    media::TTSEngine tts;
    auto voice = [&]() { need(tts.initialize()); };

    if (sub == "speech-add" || sub == "voice-add") {
      // There is no microphone path here: the "speech" is text= synthesized,
      // then heard back by the recogniser, and the task is what it heard.
      std::string text = kv(a, "text", "");
      if (text.empty()) fail(Status::Invalid, "task speech-add text=<what to say> [customer=<cid>] [priority=Low|Medium|High|Critical]");
      auto prio_res = userland::task_priority_from_string(kv(a, "priority", "Medium"));
      userland::TaskPriority prio = prio_res.ok() ? *prio_res : userland::TaskPriority::Medium;
      voice();
      auto audio_res = need(tts.synthesize(text));
      media::STTEngine stt;
      need(stt.initialize());
      userland::TaskList task_list;
      auto heard = need(task_list.add_task_from_speech(stt, audio_res.audio, prio));
      std::string task_id = need(ledger.create_task(kv(a, "customer", "default"), heard.title,
                                                    "from speech: text=" + text, (uint32_t)prio));
      c.produced(task_id);
      c.o << c.o.style(Green()) << "[SPEECH TASK CREATED]" << c.o.style(Reset()) << " ID: " << c.o.style(Cyan())
          << task_id << c.o.style(Reset()) << " heard: '" << heard.title << "' (said: '" << text << "')\n";
    } else if (sub == "speech-update" || sub == "voice-update") {
      if (a.size() < 3) fail(Status::Invalid, "task speech-update <task_id> text=<update_speech>");
      std::string task_id = c.r.expand(a[2]);
      auto rec = need(ledger.get_task(task_id));
      std::string text = kv(a, "text", "");
      if (text.empty()) fail(Status::Invalid, "task speech-update <task_id> text=<what to say, e.g. 'mark it complete'>");
      userland::TaskList task_list;
      need(task_list.add_task(as_item(rec)));
      voice();
      auto audio_res = need(tts.synthesize(text));
      media::STTEngine stt;
      need(stt.initialize());
      auto updated = need(task_list.update_task_from_speech(stt, task_id, audio_res.audio));
      if (updated.status != rec.status) {
        need(ledger.update_status(task_id, updated.status, "speech: " + text));
        c.o << c.o.style(Green()) << "[SPEECH TASK UPDATED]" << c.o.style(Reset()) << " " << task_id << ": "
            << userland::task_status_to_string(rec.status) << " -> " << userland::task_status_to_string(updated.status)
            << " (written to the ledger)\n";
      } else {
        c.o << c.o.style(Yellow()) << "[NO CHANGE]" << c.o.style(Reset()) << " " << task_id
            << ": nothing heard named a status (complete/done/finish, start/in progress, cancel); still "
            << userland::task_status_to_string(rec.status) << "\n";
      }
    } else if (sub == "speak" || sub == "tts") {
      if (a.size() < 3) fail(Status::Invalid, "task speak <task_id>");
      std::string task_id = c.r.expand(a[2]);
      auto rec = need(ledger.get_task(task_id));
      userland::TaskList task_list;
      need(task_list.add_task(as_item(rec)));
      voice();
      auto feedback = need(task_list.synthesize_task_audio_feedback(tts, task_id));
      c.o << c.o.style(Green()) << "[TTS AUDIO FEEDBACK]" << c.o.style(Reset()) << " Task " << task_id << " '"
          << rec.name << "' (" << userland::task_status_to_string(rec.status) << "): synthesized "
          << feedback.character_count << " chars into " << feedback.audio.frame_count() << " audio frames in "
          << feedback.synthesis_time_ms << " ms\n";
    } else {
      auto tasks = ledger.list_tasks(kv(a, "customer", ""), kv(a, "status", ""));
      if (tasks.empty()) {
        c.o << "the ledger holds no tasks" << (has_kv(a, "customer") || has_kv(a, "status") ? " matching that" : "")
            << "; nothing to speak\n";
        return;
      }
      userland::TaskList task_list;
      for (const auto& t : tasks) need(task_list.add_task(as_item(t)));
      voice();
      auto feedback = need(task_list.synthesize_task_list_audio_feedback(tts));
      c.o << c.o.style(Green()) << "[TTS TASK LIST FEEDBACK]" << c.o.style(Reset()) << " " << tasks.size()
          << " ledger task(s): synthesized " << feedback.audio.data.size() << " bytes, " << feedback.character_count
          << " chars\n";
    }
  } else {
    fail(Status::Invalid, "task create customer=<cid> name=<name> payload=<payload> | task list [customer=<cid>] [status=<st>] | task status <task_id> <status> | task ledger [customer=<cid>] [task_id=<id>] | task show <task_id> | task speech-add [text=<text>] | task speech-update <task_id> [text=<text>] | task speak <task_id> | task speak-list");
  }
}

void cmd_voice(Ctx& c, const Args& a) {
  if (a.size() < 2) {
    fail(Status::Invalid, "voice listen [dur=<sec>] | voice speak <text...> [out=<f.wav>] | voice hear <f.wav> | voice converse [prompt=<text>] | voice status | voice voices");
  }

  std::string sub = a[1];
  media::VoiceEngine voice;
  media::VoiceConfig cfg;
  cfg.sample_rate = 16000;
  // The engine comes up with whichever halves this machine has; speaking
  // refuses later, with the reason, on a machine that cannot.
  auto ready = voice.initialize(cfg);
  if (!ready.ok() && sub != "status" && sub != "voices") need(ready);
  // The mind answers, not the voice engine. It used to carry a handful of
  // canned sentences of its own and speak those.
  voice.set_responder([&c](const std::string& heard) -> Result<std::string> {
    std::string answer;
    auto r = ask(c.s, heard, "mind", 8, &answer);
    if (!r.ok()) return r.error();
    if (answer.empty()) return Result<std::string>(Status::NotFound, "the mind gave no answer to say");
    return answer;
  });

  if (sub == "listen") {
    uint32_t dur_ms = 3000;
    for (size_t i = 2; i < a.size(); ++i) {
      if (a[i].rfind("dur=", 0) == 0) dur_ms = (uint32_t)(to_f64(a[i].substr(4)) * 1000);
    }
    auto transcript = need(voice.listen(dur_ms));
    if (transcript.empty()) {
      c.o << "[VOICE] " << dur_ms << " ms recorded; the recogniser heard nothing said\n";
    } else {
      c.o << c.o.style(Green()) << "[VOICE HEARD]" << c.o.style(Reset()) << " '" << transcript << "'\n";
    }
  } else if (sub == "speak" || sub == "say") {
    if (a.size() < 3) fail(Status::Invalid, "voice speak <text...> [out=<file.wav>]");
    std::string text, out_path = kv(a, "out", "");
    for (size_t i = 2; i < a.size(); ++i) {
      if (a[i].rfind("out=", 0) == 0) continue;
      if (!text.empty()) text += " ";
      text += a[i];
    }
    // out= keeps the audio, so what the machine said can be listened to, moved
    // to another machine, or handed to a recogniser.
    if (!out_path.empty()) {
      auto spoken = need(voice.tts().synthesize(text));
      const auto& buf = spoken.audio;
      const int16_t* pcm = reinterpret_cast<const int16_t*>(buf.data.data());
      auto wav = hal::wave_encode(pcm, buf.data.size() / sizeof(int16_t), buf.spec.sample_rate, 1);
      need(c.k.host().write_file(out_path, ByteSpan(wav.data(), wav.size())));   // host-io: a file the operator named on the command line
      c.o << c.o.style(Green()) << "[VOICE WROTE]" << c.o.style(Reset()) << " "
          << wav.size() << " bytes of speech to " << out_path << " ("
          << fmt_double(buf.duration_seconds(), 8, true, 2) << "s at " << buf.spec.sample_rate << " Hz)\n";
      return;
    }
    auto audio = need(voice.speak(text));
    c.o << c.o.style(Green()) << "[VOICE SPOKEN]" << c.o.style(Reset())
        << " " << audio.frame_count() << " frames at " << audio.spec.sample_rate << " Hz ("
        << fmt_double(audio.duration_seconds(), 8, true, 2) << "s) through this machine's speaker\n";
  } else if (sub == "hear" || sub == "transcribe") {
    // Recognition on a recording, so the recogniser can be driven without a
    // microphone — and so what one machine said can be checked by another.
    if (a.size() < 3) fail(Status::Invalid, "voice hear <file.wav>");
    auto raw = need(c.k.host().read_file(c.r.expand(a[2])));   // host-io: a file the operator named on the command line
    auto wav = hal::wave_decode(raw.data(), raw.size());
    if (!wav.ok()) fail(wav.status(), wav.error().message);
    c.o << "  " << W{18} << "Recording" << ": " << wav->pcm.size() << " frames at " << wav->sample_rate
        << " Hz (" << fmt_double((double)wav->pcm.size() / (double)(wav->sample_rate ? wav->sample_rate : 1), 8, true, 2)
        << "s)\n";
    c.o.flush();
    auto heard = need(voice.stt().transcribe_pcm16(wav->pcm.data(), wav->pcm.size(), wav->sample_rate));
    if (heard.transcript.empty()) {
      c.o << "  " << W{18} << "Heard" << ": nothing was said\n";
    } else {
      c.o << "  " << W{18} << "Heard" << ": " << c.o.style(Green()) << heard.transcript << c.o.style(Reset()) << "\n";
      for (const auto& t : heard.tokens) {
        c.o << "      " << t.start_ms << "-" << t.end_ms << " ms  " << t.text << "\n";
      }
    }
    c.o << "  " << W{18} << "Took" << ": " << heard.processing_time_ms << " ms\n";
  } else if (sub == "chat" || sub == "converse") {
    std::string prompt;
    for (size_t i = 2; i < a.size(); ++i) {
      if (a[i].rfind("prompt=", 0) == 0) prompt = a[i].substr(7);
      else {
        if (!prompt.empty()) prompt += " ";
        prompt += a[i];
      }
    }
    if (prompt.empty()) {
      prompt = need(voice.listen(3000));
    }
    auto resp = need(voice.process_voice_interaction(prompt));
    c.o << c.o.style(Green()) << "[VOICE DUPLEX CHAT]" << c.o.style(Reset())
        << " User spoken: '" << prompt << "' -> AIOS spoken back: '" << resp.text << "'\n";
  } else if (sub == "status") {
    // What this machine can actually do with sound. The old status printed
    // "STT Engine: Active", "TTS Engine: Active" and "Audio Hardware:
    // ALSA/Pulse PCM Duplex Active" on any machine, including one with no
    // sound hardware and no engines at all.
    auto seq_res = voice.pull_shared_state();
    c.o << "Voice engine state  : " << media::voice_state_to_string(voice.state()) << "\n"
        << "Sample rate         : " << voice.config().sample_rate << " Hz\n";
    c.o << "Synthesizer         : "
        << (voice.can_speak() ? std::to_string(voice.tts().available_voices().size()) +
                                    " voice(s) from the substrate's engine"
                              : "none — " + voice.synthesis_error()) << "\n";
    auto rec = hal::speech_recognizer();
    c.o << "Recogniser          : " << (rec.ok() ? *rec : "none — " + rec.error().message) << "\n";
    auto ends = hal::audio_endpoints();
    if (!ends.ok()) {
      c.o << "Audio hardware      : none — " << ends.error().message << "\n";
    } else if (ends->empty()) {
      c.o << "Audio hardware      : none found on this machine\n";
    } else {
      for (const auto& e : *ends) {
        c.o << "Audio hardware      : " << (e.input ? "in  " : "out ") << e.name
            << (e.is_default ? "  (default)" : "") << "\n";
      }
    }
    c.o << "Shared state region : " << (seq_res.ok() ? "reading (seq " + std::to_string(*seq_res) + ")" : "offline") << "\n";
  } else if (sub == "shm" || sub == "pull") {
    float state_buf[intelligence::kSubstrateStateDim];
    float action_buf[intelligence::kActionVectorDim];
    float next_buf[intelligence::kSubstrateStateDim];
    auto seq_res = need(voice.pull_shared_state(state_buf, action_buf, next_buf));
    c.o << "[DUPLEX VOICE SHARED MEMORY PULL]\n"
        << "Atomic Sequence: " << seq_res << "\n"
        << "Pulled Substrate State Vector Dimension: " << intelligence::kSubstrateStateDim << " floats\n"
        << "Pulled Action Vector Dimension: " << intelligence::kActionVectorDim << " floats\n"
        << "First 4 Latent Components: [" << state_buf[0] << ", " << state_buf[1] << ", " << state_buf[2] << ", " << state_buf[3] << "...]\n";
  } else if (sub == "voices") {
    auto voices = voice.tts().available_voices();
    if (voices.empty()) { c.o << "no voices: " << voice.synthesis_error() << "\n"; return; }
    c.o << voices.size() << " voice(s), from this machine's own speech engine:\n";
    for (const auto& v : voices) {
      c.o << "  " << W{22} << v.name << v.language << (v.gender.empty() ? "" : ", " + v.gender) << "\n";
    }
  } else {
    fail(Status::Invalid, "voice listen [dur=<sec>] | voice speak <text...> [out=<f.wav>] | voice hear <f.wav> | voice converse [prompt=<text>] | voice status | voice shm | voice voices");
  }
}

// Git is not a program this OS shells out to: the object database, the SHA-1
// naming, the zlib envelope and the refs are all kernel userland (see
// aios/userland/git.hpp). This command is only the surface over it.
std::string git_when(uint64_t secs, const std::string& tz) {
  // Render in the offset the commit itself carries, the way git shows it.
  int64_t off = 0;
  if (tz.size() >= 5 && (tz[0] == '+' || tz[0] == '-')) {
    int64_t hh = (tz[1] - '0') * 10 + (tz[2] - '0');
    int64_t mm = (tz[3] - '0') * 10 + (tz[4] - '0');
    off = (hh * 3600 + mm * 60) * (tz[0] == '-' ? -1 : 1);
  }
  int64_t z = (int64_t)secs + off;
  int64_t sod = z % 86400; if (sod < 0) sod += 86400;
  int64_t days = (z - sod) / 86400 + 719468;
  int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  int64_t doe = days - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  int64_t d = doy - (153 * mp + 2) / 5 + 1;
  int64_t m = mp + (mp < 10 ? 3 : -9);
  y += (m <= 2);
  auto pad2 = [](int64_t v) { std::string s = fmt_u64((uint64_t)v); return s.size() < 2 ? "0" + s : s; };
  return fmt_u64((uint64_t)y) + "-" + pad2(m) + "-" + pad2(d) + " " +
         pad2(sod / 3600) + ":" + pad2((sod % 3600) / 60) + ":" + pad2(sod % 60) + " " + tz;
}


// ---------------------------------------------------------------------------
// ssh — this machine's own client, not a program it runs
// ---------------------------------------------------------------------------
// The identity and the known-hosts file are state OF THE MACHINE: they live
// under the kernel root, so a checkpointed root carries them and every entity
// on this kernel meets the same hosts under the same key.
std::string ssh_key_path(Kernel& k) { return k.config().root + "/ssh/id_ed25519"; }
std::string ssh_known_hosts(Kernel& k) { return k.config().root + "/ssh/known_hosts"; }

void need_network(Surface& actor) {
  // Reaching out to a peer is network:consume, the same right `node connect`
  // spends — an ssh session and a clone are not a different kind of network.
  if (auto r = actor.may(ResourceRef{ResourceKind::Network, {}}, R_Consume); !r.ok())
    fail(Status::Denied, "entity " + actor.self().short_str() + " may not reach the network: " + r.error().str());
}

net::SshIdentity load_or_make_identity(Ctx& c, Surface& actor, const std::string& path, bool make_if_missing,
                                       const std::string& passphrase = {}) {
  // The key material lives in the network layer; the console only decides who
  // is asking. Writing one is storage, and the kernel says whether this entity may.
  if (make_if_missing)
    if (auto r = actor.may(ResourceRef{ResourceKind::StorageObject, {}}, R_Write); !r.ok())
      fail(Status::Denied, "entity " + actor.self().short_str() + " may not write a key: " + r.error().str());
  auto id = net::ssh_load_identity(c.k.host(), path, make_if_missing, "aios-" + c.k.self().short_str(), passphrase);
  if (!id.ok()) fail(id.error().status, id.error().message + (make_if_missing ? "" : " (run `ssh keygen`)"));
  return *id;
}

void cmd_ssh(Ctx& c, const Args& a) {
  c.view();
  Surface actor = has_kv(a, "as") ? c.as(kv(a, "as")) : c.me;
  std::string sub = (a.size() > 1) ? a[1] : "help";
  std::string key = has_kv(a, "key") ? c.r.expand(kv(a, "key")) : ssh_key_path(c.k);
  // An encrypted key's passphrase: passphrase=@FILE or passphrase=env:NAME,
  // else AIOS_SSH_PASSPHRASE. Never the passphrase itself on the line: the
  // command lands in the console transcript, the shell pane and session.log.
  std::string passphrase;
  if (has_kv(a, "passphrase")) {
    const std::string src = kv(a, "passphrase");
    if (src.rfind("env:", 0) != 0 && (src.empty() || src[0] != '@'))
      fail(Status::Invalid, "passphrase= takes @FILE or env:NAME, not the passphrase itself (it would be kept in the transcript)");
    passphrase = fleet_key_arg(src);
  } else if (const char* env = std::getenv("AIOS_SSH_PASSPHRASE")) {
    passphrase = env;
  }

  if (sub == "keygen" || sub == "key") {
    std::string path = key;
    for (size_t i = 2; i < a.size(); ++i)
      if (a[i].find('=') == std::string::npos) { path = c.r.expand(a[i]); break; }
    bool existed = net::ssh_load_identity(c.k.host(), path, false).ok();
    auto id = load_or_make_identity(c, actor, path, true, passphrase);
    c.o << c.o.style(existed ? Header() : Green()) << (existed ? "[SSH KEY]" : "[SSH KEYGEN]") << c.o.style(Reset())
        << " " << (id.type == "ssh-rsa" ? "rsa" : "ed25519") << " identity of kernel " << c.k.self().short_str() << " at " << c.o.style(Cyan()) << path << c.o.style(Reset())
        << (existed ? " (already held)" : " (generated from this machine's randomness)") << "\n";
    c.o << "  " << W{14} << "fingerprint" << ": " << id.fingerprint() << "\n";
    c.o << "  " << W{14} << "public key" << ": " << id.public_line() << "\n";
    c.o << "  put that line in the remote account's authorized_keys and this machine can reach it\n";
    c.produced(id.fingerprint());
    return;
  }

  if (sub == "help" || sub == "--help" || sub == "-h") {
    c.o << c.o.style(Header()) << "AIOS SSH Subsystem" << c.o.style(Reset()) << "\n\n"
        << "Usage:\n"
        << "  ssh <user@host[:port]> <command...> [key=<path>] [passphrase=@FILE|env:NAME] [as=$entity]\n"
        << "                                  Execute a remote command over SSH-2\n"
        << "  ssh keygen [path]               Generate or display root SSH keypair\n"
        << "  ssh known                       List known remote host fingerprints\n";
    return;
  }

  if (sub == "known" || sub == "hosts") {
    std::string path = ssh_known_hosts(c.k);
    c.o << c.o.style(Header()) << "Hosts this machine has met (" << path << ")" << c.o.style(Reset()) << "\n";
    auto known = need(net::ssh_known_hosts(c.k.host(), path));
    if (known.empty()) { c.o << "  (none yet)\n"; return; }
    for (auto& entry : known)
      c.o << "  " << W{28} << entry.host << W{16} << entry.algorithm << entry.fingerprint << "\n";
    return;
  }

  if (a.size() < 3) fail(Status::Invalid, "ssh <user@host[:port]> <command...> [key=<path>] [passphrase=@FILE|env:NAME] [as=$entity] | ssh keygen | ssh known");
  need_network(actor);
  auto target = net::ssh_parse_target(c.r.expand(a[1]), "");
  if (!target.ok()) fail(target.error().status, target.error().message);
  std::string command;
  for (size_t i = 2; i < a.size(); ++i) {
    if (a[i].rfind("key=", 0) == 0 || a[i].rfind("as=", 0) == 0 || a[i].rfind("passphrase=", 0) == 0) continue;
    command += (command.empty() ? "" : " ") + c.r.expand(a[i]);
  }
  if (command.empty()) fail(Status::Invalid, "ssh <user@host> <command...> — this machine runs a command, it has no terminal to give you");

  auto id = load_or_make_identity(c, actor, key, true, passphrase);
  net::SshSession session(c.k.host(), ssh_known_hosts(c.k));
  need(session.connect(*target, id, 20000));
  c.o << c.o.style(Header()) << "ssh " << target->user << "@" << target->host << ":" << target->port
      << c.o.style(Reset()) << "\n";
  c.o << "  " << W{14} << "server" << ": " << session.server_version() << "\n";
  c.o << "  " << W{14} << "host key" << ": " << session.host_key().fingerprint
      << (session.host_key().first_seen ? "  (first time — recorded)" : "  (known)") << "\n";
  c.o << "  " << W{14} << "negotiated" << ": " << session.kex() << " + " << session.cipher() << "\n";
  c.o << "  " << W{14} << "identity" << ": " << id.fingerprint() << "\n";
  need(session.exec(command));
  need(session.send_eof());
  std::string out;
  while (!session.eof()) {
    auto chunk = session.recv(20000);
    if (!chunk.ok()) fail(chunk.error().status, chunk.error().message);
    out.append((const char*)chunk->data(), chunk->size());
  }
  need(session.wait_close(20000));      // the exit status comes after the last byte
  c.o << c.o.style(Bold()) << "  $ " << command << c.o.style(Reset()) << "\n";
  for (auto& line : split_text_lines(out)) c.o << "  " << line << "\n";
  if (!session.stderr_text().empty())
    for (auto& line : split_text_lines(session.stderr_text())) c.o << "  " << c.o.style(Red()) << line << c.o.style(Reset()) << "\n";
  c.o << "  " << W{14} << "exit" << ": " << session.exit_status() << "   " << session.bytes_in() << " bytes in, "
      << session.bytes_out() << " bytes out\n";
  c.produced(out.empty() ? std::string("(no output)") : split_text_lines(out).front());
  if (session.exit_status() > 0)
    fail(Status::IoError, "the remote command exited " + fmt_u64((uint64_t)session.exit_status()));
}

void cmd_tx(Ctx& c, const Args& a) {
  c.view();
  if (a.size() < 2 || a[1] == "help" || a[1] == "--help") {
    c.o << "Transaction Engine (AIOS Atomic Multi-File Diff Transactions)\n\n"
        << "Usage:\n"
        << "  tx plan <patch_file>   Parse diff and display Intent Declaration plan manifest\n"
        << "  tx stage <patch_file>  Dry-run pre-flight check and stage in memory\n"
        << "  tx commit              Commit staged transaction atomically to disk\n"
        << "  tx abort               Abort staged transaction and discard memory workspace\n"
        << "  tx show                Show currently staged transaction details\n"
        << "  tx review              Print the staged diff hunk by hunk (desktop editor: Ctrl+L `review`)\n"
        << "  tx apply <patch_file>  Dry-run, stage and commit patch atomically in one step\n";
    return;
  }

  std::string sub = a[1];
  if (sub == "plan") {
    if (a.size() < 3) fail(Status::Invalid, "Usage: tx plan <diff_file_or_patch>");
    std::string path = a[2];
    auto rd = c.k.host().read_file(path);   // host-io: the patch file the operator named (tx)
    if (!rd.ok()) fail(Status::NotFound, "Cannot read patch file: " + path);
    std::string text(reinterpret_cast<const char*>(rd->data()), rd->size());
    auto parsed = userland::PatchEngine::parse_unified_diff(text);
    if (!parsed.ok()) fail(parsed.status(), "Patch parse error: " + parsed.error().str());
    auto manifest = *parsed;
    auto dry = userland::PatchEngine::dry_run(c.k, manifest);
    c.o << userland::PatchEngine::format_plan_manifest(manifest) << "\n";
    if (dry.ok()) {
      c.o << c.o.style(Green()) << "[SUCCESS] Pre-flight dry-run check PASSED ("
          << manifest.files.size() << " files clean, "
          << manifest.total_hunks() << " hunks match substrate)" << c.o.style(Reset()) << "\n";
    } else {
      c.o << c.o.style(Red()) << "[REJECTED] Pre-flight dry-run check FAILED: "
          << dry.error().str() << c.o.style(Reset()) << "\n";
    }
  } else if (sub == "stage") {
    if (a.size() < 3) fail(Status::Invalid, "Usage: tx stage <diff_file_or_patch>");
    std::string path = a[2];
    auto rd = c.k.host().read_file(path);   // host-io: the patch file the operator named (tx)
    if (!rd.ok()) fail(Status::NotFound, "Cannot read patch file: " + path);
    std::string text(reinterpret_cast<const char*>(rd->data()), rd->size());
    auto parsed = userland::PatchEngine::parse_unified_diff(text);
    if (!parsed.ok()) fail(parsed.status(), "Patch parse error: " + parsed.error().str());
    auto manifest = *parsed;
    auto res = userland::PatchEngine::stage(c.k, manifest);
    if (!res.ok()) fail(res.status(), "Staging failed: " + res.error().str());
    c.o << c.o.style(Green()) << "[STAGED] Transaction #" << manifest.tx_id << ": " << manifest.title
        << " (" << manifest.files.size() << " files, " << manifest.total_hunks() << " hunks staged in memory)"
        << c.o.style(Reset()) << "\n"
        << "Main tree is pristine. Read it with `tx review`, or step its hunks in the desktop editor\n"
        << "(Ctrl+L, then `review`); commit with `tx commit`. Staging lives in this session only.\n";
  } else if (sub == "commit") {
    if (!userland::PatchEngine::has_active_transaction()) {
      fail(Status::Invalid, "No active staged transaction. Use `tx stage <patch>` first.");
    }
    auto& manifest = userland::PatchEngine::active_transaction();
    auto res = userland::PatchEngine::commit(c.k, manifest);
    if (!res.ok()) fail(res.status(), "Commit failed: " + res.error().str());
    c.o << c.o.style(Green()) << "[COMMITTED] Transaction #" << manifest.tx_id
        << " atomically written to disk (" << manifest.files.size() << " files updated)."
        << c.o.style(Reset()) << "\n";
  } else if (sub == "abort") {
    if (!userland::PatchEngine::has_active_transaction()) {
      c.o << "No active staged transaction to abort.\n";
      return;
    }
    auto& manifest = userland::PatchEngine::active_transaction();
    uint64_t id = manifest.tx_id;
    userland::PatchEngine::abort(manifest);
    c.o << c.o.style(Yellow()) << "[ABORTED] Transaction #" << id
        << " staging discarded. Disk tree untouched." << c.o.style(Reset()) << "\n";
  } else if (sub == "review") {
    // The staged diff itself, hunk by hunk, with what is accepted. The
    // interactive version (toggle hunks, commit/abort) is the desktop editor's
    // `review` command, which opens this same transaction.
    if (!userland::PatchEngine::has_active_transaction()) {
      fail(Status::Invalid, "No active staged transaction. Use `tx stage <patch>` first (in this session).");
    }
    auto& manifest = userland::PatchEngine::active_transaction();
    c.o << c.o.style(Header()) << "Review: Transaction #" << manifest.tx_id << " - " << manifest.title
        << c.o.style(Reset()) << "\n";
    int n = 0;
    for (const auto& f : manifest.files) {
      c.o << c.o.style(Bold()) << userland::file_op_symbol(f.op_type) << " "
          << (f.new_path.empty() ? f.old_path : f.new_path) << c.o.style(Reset())
          << "  (+" << f.additions_count << " -" << f.deletions_count << ")\n";
      for (const auto& h : f.hunks) {
        c.o << "  hunk " << n++ << " [" << (h.accepted ? "accepted" : "rejected") << "] " << h.header << "\n";
        for (const auto& l : h.lines) {
          if (l.type == userland::DiffLineType::HunkHeader || l.type == userland::DiffLineType::FileHeader) continue;
          const char pre = l.type == userland::DiffLineType::Addition ? '+'
                         : l.type == userland::DiffLineType::Deletion ? '-' : ' ';
          if (pre == '+') c.o << c.o.style(Green());
          else if (pre == '-') c.o << c.o.style(Red());
          c.o << "    " << pre << l.content << c.o.style(Reset()) << "\n";
        }
      }
    }
    c.o << "Accepted " << manifest.active_hunks_count() << "/" << manifest.total_hunks()
        << " hunks. `tx commit` writes them; `tx abort` discards. Desktop editor: Ctrl+L `review` to toggle hunks.\n";
  } else if (sub == "show" || sub == "list" || sub == "status") {
    if (!userland::PatchEngine::has_active_transaction()) {
      c.o << "No active staged transaction in memory.\n";
      return;
    }
    auto& manifest = userland::PatchEngine::active_transaction();
    c.o << userland::PatchEngine::format_plan_manifest(manifest) << "\n"
        << "Status: " << userland::transaction_status_name(manifest.status) << "\n"
        << "Hunks: " << manifest.active_hunks_count() << "/" << manifest.total_hunks() << " accepted\n"
        << "Delta: +" << manifest.total_additions() << " -" << manifest.total_deletions() << " lines\n";
  } else if (sub == "apply") {
    if (a.size() < 3) fail(Status::Invalid, "Usage: tx apply <diff_file_or_patch>");
    std::string path = a[2];
    auto rd = c.k.host().read_file(path);   // host-io: the patch file the operator named (tx)
    if (!rd.ok()) fail(Status::NotFound, "Cannot read patch file: " + path);
    std::string text(reinterpret_cast<const char*>(rd->data()), rd->size());
    auto res = userland::PatchEngine::apply_patch_atomic(c.k, text);
    if (!res.ok()) fail(res.status(), "Apply error: " + res.error().str());
    c.o << c.o.style(Green()) << "[COMMITTED] Applied atomic patch from " << path
        << c.o.style(Reset()) << "\n";
  } else {
    fail(Status::Invalid, "Unknown tx subcommand: " + sub);
  }
}

void cmd_npm(Ctx& c, const Args& a) {
  c.view();
  Surface actor = has_kv(a, "as") ? c.as(kv(a, "as")) : c.me;
  std::string sub = (a.size() > 1) ? a[1] : "help";
  userland::NpmOptions opt;
  if (has_kv(a, "registry")) opt.registry = c.r.expand(kv(a, "registry"));
  if (has_kv(a, "timeout")) opt.timeout_ms = (int)to_u64(kv(a, "timeout"));
  if (has_kv(a, "deps")) opt.with_dependencies = kv(a, "deps") != "0" && kv(a, "deps") != "no";
  if (has_kv(a, "max")) opt.max_packages = (size_t)to_u64(kv(a, "max"));
  userland::Npm npm(c.k, &actor);

  // Where a package tree belongs: the repository this kernel is standing in,
  // unless the caller names somewhere else.
  auto project_dir = [&]() -> std::string {
    if (has_kv(a, "dir")) return c.r.expand(kv(a, "dir"));
    userland::GitRegistry reg(actor);
    if (reg.attach().ok()) { auto cur = reg.current(); if (cur.ok()) return *cur; }
    return c.k.config().root;
  };

  if (sub == "view" || sub == "info" || sub == "versions") {
    if (a.size() < 3) fail(Status::Invalid, "npm view <package>");
    std::string latest;
    auto vs = need(npm.versions(c.r.expand(a[2]), latest, opt));
    c.o << c.o.style(Header()) << c.r.expand(a[2]) << " — " << vs.size() << " published version(s), latest "
        << c.o.style(Cyan()) << latest << c.o.style(Reset()) << "\n";
    size_t shown = 0;
    for (size_t i = vs.size(); i-- > 0 && shown < 12; ++shown) c.o << "  " << vs[i] << "\n";
    if (vs.size() > shown) c.o << "  … and " << (vs.size() - shown) << " older\n";
    c.produced(latest);
    return;
  }

  if (sub == "install" || sub == "i" || sub == "add") {
    std::string spec;
    for (size_t i = 2; i < a.size(); ++i)
      if (a[i].find('=') == std::string::npos) { spec = c.r.expand(a[i]); break; }
    std::string dir = project_dir();
    auto rep = need(npm.install(dir, spec, opt));
    c.o << c.o.style(Green()) << "[NPM INSTALL]" << c.o.style(Reset()) << " "
        << (spec.empty() ? std::string("everything ") + dir + "/package.json asks for" : spec) << "\n";
    c.o << "  " << W{14} << "registry" << ": " << rep.registry << " over " << rep.tls << "\n";
    c.o << "  " << W{14} << "trusted via" << ": " << rep.trust_anchor
        << " — every tarball checked against the hash the registry published\n";
    c.o << "  " << W{14} << "into" << ": " << rep.root << "\n";
    for (auto& p : rep.installed)
      c.o << "  " << W{28} << p.name << W{12} << p.version << W{8} << (fmt_u64(p.files) + " files") << "  "
          << fmt_bytes(p.bytes) << "\n";
    c.o << "  " << W{14} << "total" << ": " << rep.installed.size() << " package(s), " << fmt_bytes(rep.downloaded_bytes)
        << " downloaded in " << fmt_ns(rep.took_ns) << "\n";
    if (!rep.installed.empty()) c.produced(rep.installed.front().version);
    return;
  }

  fail(Status::Invalid, "npm install [<package>[@range]] [dir=<path>] [registry=<url>] [deps=0] | npm view <package>  [as=$entity]");
}

void cmd_pip(Ctx& c, const Args& a) {
  c.view();
  Surface actor = has_kv(a, "as") ? c.as(kv(a, "as")) : c.me;
  std::string sub = (a.size() > 1) ? a[1] : "help";
  userland::PyOptions opt;
  if (has_kv(a, "index")) opt.index = c.r.expand(kv(a, "index"));
  if (has_kv(a, "python")) opt.python = c.r.expand(kv(a, "python"));
  if (has_kv(a, "platform")) opt.platform = c.r.expand(kv(a, "platform"));
  if (has_kv(a, "timeout")) opt.timeout_ms = (int)to_u64(kv(a, "timeout"));
  if (has_kv(a, "deps")) opt.with_dependencies = kv(a, "deps") != "0" && kv(a, "deps") != "no";
  userland::PyPi pip(c.k, &actor);

  auto where = [&]() -> std::string {
    if (has_kv(a, "dir")) return c.r.expand(kv(a, "dir"));
    userland::GitRegistry reg(actor);
    if (reg.attach().ok()) { auto cur = reg.current(); if (cur.ok()) return *cur + "/site-packages"; }
    return c.k.config().root + "/site-packages";
  };

  if (sub == "view" || sub == "info" || sub == "versions") {
    if (a.size() < 3) fail(Status::Invalid, "pip view <package>");
    std::string latest;
    auto vs = need(pip.versions(c.r.expand(a[2]), latest, opt));
    c.o << c.o.style(Header()) << c.r.expand(a[2]) << " — " << vs.size() << " release(s), latest "
        << c.o.style(Cyan()) << latest << c.o.style(Reset()) << "\n";
    c.produced(latest);
    return;
  }
  if (sub == "install" || sub == "i" || sub == "add") {
    std::string spec;
    for (size_t i = 2; i < a.size(); ++i)
      if (a[i].find('=') == std::string::npos || a[i].find("==") != std::string::npos) { spec = c.r.expand(a[i]); break; }
    std::string dir = where();
    auto rep = need(pip.install(dir, spec, opt));
    c.o << c.o.style(Green()) << "[PIP INSTALL]" << c.o.style(Reset()) << " "
        << (spec.empty() ? std::string("everything requirements.txt asks for") : spec) << "\n";
    c.o << "  " << W{14} << "index" << ": " << rep.index << " over " << rep.tls << "\n";
    c.o << "  " << W{14} << "trusted via" << ": " << rep.trust_anchor
        << " — every wheel checked against the sha256 the index published\n";
    c.o << "  " << W{14} << "chosen for" << ": " << rep.environment << "\n";
    c.o << "  " << W{14} << "into" << ": " << rep.target << "\n";
    for (auto& p : rep.installed) {
      c.o << "  " << W{26} << p.name << W{12} << p.version << W{10} << (fmt_u64(p.files) + " files") << "  "
          << W{10} << fmt_bytes(p.bytes) << "  " << p.wheel << "\n";
    }
    c.o << "  " << W{14} << "total" << ": " << rep.installed.size() << " package(s), " << fmt_bytes(rep.downloaded_bytes)
        << " in " << fmt_ns(rep.took_ns) << "\n";
    if (!rep.installed.empty()) c.produced(rep.installed.front().version);
    return;
  }
  fail(Status::Invalid, "pip install <package>[==version] [dir=<path>] [python=3.12] [platform=x86_64] [deps=0] | pip view <package>");
}

void cmd_cargo(Ctx& c, const Args& a) {
  c.view();
  Surface actor = has_kv(a, "as") ? c.as(kv(a, "as")) : c.me;
  std::string sub = (a.size() > 1) ? a[1] : "help";
  userland::CrateOptions opt;
  if (has_kv(a, "registry")) opt.registry = c.r.expand(kv(a, "registry"));
  if (has_kv(a, "timeout")) opt.timeout_ms = (int)to_u64(kv(a, "timeout"));
  if (has_kv(a, "deps")) opt.with_dependencies = kv(a, "deps") != "0" && kv(a, "deps") != "no";
  userland::Crates crates(c.k, &actor);

  auto where = [&]() -> std::string {
    if (has_kv(a, "dir")) return c.r.expand(kv(a, "dir"));
    userland::GitRegistry reg(actor);
    if (reg.attach().ok()) { auto cur = reg.current(); if (cur.ok()) return *cur; }
    return c.k.config().root;
  };

  if (sub == "view" || sub == "info" || sub == "versions") {
    if (a.size() < 3) fail(Status::Invalid, "cargo view <crate>");
    std::string latest;
    auto vs = need(crates.versions(c.r.expand(a[2]), latest, opt));
    c.o << c.o.style(Header()) << c.r.expand(a[2]) << " — " << vs.size() << " version(s), newest stable "
        << c.o.style(Cyan()) << latest << c.o.style(Reset()) << "\n";
    c.produced(latest);
    return;
  }
  if (sub == "vendor" || sub == "add" || sub == "fetch") {
    if (a.size() < 3) fail(Status::Invalid, "cargo vendor <crate>[@version] [dir=<path>]");
    std::string spec;
    for (size_t i = 2; i < a.size(); ++i)
      if (a[i].find('=') == std::string::npos) { spec = c.r.expand(a[i]); break; }
    auto rep = need(crates.vendor(where(), spec, opt));
    c.o << c.o.style(Green()) << "[CARGO VENDOR]" << c.o.style(Reset()) << " " << spec << "\n";
    c.o << "  " << W{14} << "registry" << ": " << rep.registry << " over " << rep.tls << "\n";
    c.o << "  " << W{14} << "trusted via" << ": " << rep.trust_anchor
        << " — every archive checked against the checksum the registry published\n";
    c.o << "  " << W{14} << "into" << ": " << rep.target << "\n";
    for (auto& p : rep.vendored)
      c.o << "  " << W{24} << p.name << W{12} << p.version << W{10} << (fmt_u64(p.files) + " files") << "  "
          << fmt_bytes(p.bytes) << "\n";
    c.o << "  " << W{14} << "total" << ": " << rep.vendored.size() << " crate(s), " << fmt_bytes(rep.downloaded_bytes)
        << " in " << fmt_ns(rep.took_ns) << " — source, verified; building it is a compiler's job\n";
    if (!rep.vendored.empty()) c.produced(rep.vendored.front().version);
    return;
  }
  fail(Status::Invalid, "cargo vendor <crate>[@version] [dir=<path>] [deps=0] | cargo view <crate>");
}

// How long between one run of a standing directive and the next. Written the
// way a person says it, because a person is giving the order.
static uint64_t parse_interval(const std::string& text, bool& ok) {
  ok = false;
  uint64_t n = 0;
  size_t i = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') { n = n * 10 + (uint64_t)(text[i] - '0'); ++i; }
  if (i == 0) return 0;
  std::string unit = text.substr(i);
  ok = true;
  if (unit.empty() || unit == "s" || unit == "sec") return n * 1000;
  if (unit == "ms") return n;
  if (unit == "m" || unit == "min") return n * 60000;
  if (unit == "h" || unit == "hour") return n * 3600000;
  if (unit == "d" || unit == "day") return n * 86400000ull;
  if (unit == "once") return 0;
  ok = false;
  return 0;
}

static std::string fmt_interval(uint64_t ms) {
  if (!ms) return "once";
  if (ms % 86400000ull == 0) return std::to_string(ms / 86400000ull) + "d";
  if (ms % 3600000 == 0) return std::to_string(ms / 3600000) + "h";
  if (ms % 60000 == 0) return std::to_string(ms / 60000) + "m";
  if (ms % 1000 == 0) return std::to_string(ms / 1000) + "s";
  return std::to_string(ms) + "ms";
}

// A span of time a person reads at a glance: the two largest units, e.g.
// 21599999 ms -> "5h59m", 90061000 -> "1d1h", 4500 -> "4s".
static std::string fmt_span(uint64_t ms) {
  uint64_t s = ms / 1000;
  uint64_t d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60, sec = s % 60;
  if (d) return std::to_string(d) + "d" + (h ? std::to_string(h) + "h" : "");
  if (h) return std::to_string(h) + "h" + (m ? std::to_string(m) + "m" : "");
  if (m) return std::to_string(m) + "m" + (sec ? std::to_string(sec) + "s" : "");
  if (s) return std::to_string(s) + "s";
  return std::to_string(ms) + "ms";
}

// Local wall-clock time of day (HH:MM) at `wall_ns`, or "" where there is no zone.
static std::string clock_at(uint64_t wall_ns) {
#if !defined(AIOS_METAL)
  std::time_t t = (std::time_t)(wall_ns / 1000000000ull);
  std::tm tm{};
  if (localtime_r(&t, &tm) != nullptr) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
  }
#endif
  (void)wall_ns;
  return "";
}

void cmd_sense(Ctx& c, const Args& a) {
  c.view();
  if (a.size() < 2) fail(Status::Invalid, "sense <url> [as=$e] [keep=0] [max=<chars>] [show=<chars>] [links=<n>] — look at a page: what it says and where it points");
  Surface actor = has_kv(a, "as") ? c.as(kv(a, "as")) : c.me;
  userland::SenseOptions opt;
  opt.embed = &embed_text;
  // Whoever asked has to be able to READ the page from this output alone — a
  // mind sees nothing else. Six lines and no links left it guessing URLs.
  opt.collect_links = true;
  size_t show = has_kv(a, "show") ? (size_t)to_u64(kv(a, "show")) : 2400;
  size_t nlinks = has_kv(a, "links") ? (size_t)to_u64(kv(a, "links")) : 20;
  if (has_kv(a, "keep")) opt.keep = kv(a, "keep") != "0" && kv(a, "keep") != "no";
  if (has_kv(a, "max")) opt.max_text = (size_t)to_u64(kv(a, "max"));
  if (has_kv(a, "timeout")) opt.timeout_ms = (int)to_u64(kv(a, "timeout"));
  std::string url;
  for (size_t i = 1; i < a.size(); ++i)
    if (a[i].find('=') == std::string::npos || a[i].find("://") != std::string::npos) { url = c.r.expand(a[i]); break; }
  userland::Sense sense(c.k, &actor);
  auto p = need(sense.look(url, opt));
  c.o << c.o.style(Green()) << "[SENSE]" << c.o.style(Reset()) << " " << p.title << "\n";
  c.o << "  " << W{14} << "from" << ": " << p.url << " over " << p.tls
      << (p.trust_anchor.empty() ? std::string() : " — trusted via " + p.trust_anchor)
      << (p.resumed ? " (session resumed)" : "") << (p.client_certificate_sent ? " (our certificate presented)" : "") << "\n";
  c.o << "  " << W{14} << "read" << ": " << fmt_bytes(p.fetched_bytes) << " on the wire, "
      << fmt_bytes(p.text_bytes) << " of text, in " << fmt_ns(p.took_ns) << "\n";
  if (p.kept)
    c.o << "  " << W{14} << (p.already_known ? "already knew" : "now knows") << ": object " << p.object.str() << "\n";
  // What it says, up to `show` characters, so the reader really has read it.
  {
    std::string t = p.text.size() > show ? p.text.substr(0, show) : p.text;
    size_t at = 0;
    while (at < t.size()) {
      size_t nl = t.find('\n', at);
      std::string line = t.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
      if (!line.empty()) c.o << "  | " << line << "\n";
      if (nl == std::string::npos) break;
      at = nl + 1;
    }
    if (p.text.size() > show) c.o << "  | …(" << (p.text.size() - show) << " more chars: sense " << url << " show=" << p.text.size() << ")\n";
  }
  // Where it points, so the next step is a real URL and not a guess.
  if (nlinks && !p.links.empty()) {
    c.o << "  " << W{14} << "links" << ": " << p.links.size() << (p.links.size() > nlinks ? " (first " + fmt_u64(nlinks) + "; links=<n> for more)" : "") << "\n";
    size_t n = 0;
    for (const auto& l : p.links) {
      if (n++ >= nlinks) break;
      std::string t = l.text;
      for (auto& ch : t) if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
      if (t.size() > 90) t = t.substr(0, 90) + "…";
      c.o << "    -> " << (t.empty() ? "(no text)" : t) << "  " << l.url << "\n";
    }
  }
  c.produced(p.object.str());
}


// A date a person can read, from the kernel's wall clock, with no libc calendar
// underneath (this file is shared with bare metal). Days since 1970-01-01 to
// y-m-d, after Howard Hinnant's civil_from_days.
static std::string civil_date(uint64_t wall_ns) {
  auto two = [](uint64_t v) { std::string s = fmt_u64(v); return s.size() < 2 ? "0" + s : s; };
#if !defined(AIOS_METAL)
  // The date is the one someone AT this machine would write: its own zone's
  // civil day, not UTC's. Otherwise every evening run files its brief under
  // tomorrow. Bare metal has no zone database and keeps the UTC math below.
  std::time_t t = (std::time_t)(wall_ns / 1000000000ull);
  std::tm tm{};
  if (localtime_r(&t, &tm) != nullptr)
    return fmt_u64((uint64_t)(tm.tm_year + 1900)) + "-" + two((uint64_t)(tm.tm_mon + 1)) + "-" + two((uint64_t)tm.tm_mday);
#endif
  int64_t z = (int64_t)(wall_ns / 1000000000ull / 86400ull) + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  uint64_t doe = (uint64_t)(z - era * 146097);
  uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = (int64_t)yoe + era * 400;
  uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint64_t mp = (5 * doy + 2) / 153;
  uint64_t d = doy - (153 * mp + 2) / 5 + 1;
  uint64_t m = mp < 10 ? mp + 3 : mp - 9;
  if (m <= 2) ++y;
  return fmt_u64((uint64_t)y) + "-" + two(m) + "-" + two(d);
}


void cmd_directive(Ctx& c, const Args& a) {
  c.view();
  std::string sub = a.size() > 1 ? a[1] : "list";
  Surface holder = c.me;
  userland::Directives reg(holder);
  need(reg.attach());
  uint64_t now = c.k.host().wall_ns() / 1000000ull;   // directives run on the wall clock

  if (sub == "list" || sub == "show") {
    const auto& all = reg.all();
    if (all.empty()) { c.o << "no standing directives — this machine does nothing unless asked\n"; return; }
    c.o << c.o.style(Header()) << "Standing directives" << c.o.style(Reset()) << " (register state "
        << reg.state().short_str() << (reg.adopted() ? ", adopted from this kernel" : ", newly minted") << ")\n";
    c.o << "  NAME            EVERY   RUNS  NEXT              AS            LAST\n";
    const uint64_t wall_now = c.k.host().wall_ns();
    for (const auto& d : all) {
      c.o << "  " << W{16} << d.name << W{8} << fmt_interval(d.every_ms) << W{6} << d.runs;
      std::string next;
      if (!d.enabled) next = "off";
      else if (d.due_ms <= now) next = "due";
      else {
        // In how long, and at what local time.
        uint64_t wait = d.due_ms - now;
        std::string at = clock_at(wall_now + wait * 1000000ull);
        next = "in " + fmt_span(wait) + (at.empty() ? "" : " (" + at + ")");
      }
      c.o << W{18} << next << W{14} << d.as.short_str()
          << (d.last_ok ? "" : "! ") << d.last_result << "\n";
      c.o << "      " << c.o.style(Dim()) << d.line << c.o.style(Reset()) << "\n";
    }
    return;
  }
  if (sub == "add" || sub == "give") {
    if (a.size() < 5) fail(Status::Invalid, "directive add <name> <every> as=$e <command…>");
    userland::Directive d;
    d.name = c.r.expand(a[2]);
    bool ok = false;
    d.every_ms = parse_interval(c.r.expand(a[3]), ok);
    if (!ok) fail(Status::Invalid, "an interval reads like 30s, 5m, 1h, 1d or once");
    if (!has_kv(a, "as")) fail(Status::Invalid, "a directive must say whose authority it acts with: as=$entity");
    d.as = c.as(kv(a, "as")).self();
    std::string line;
    for (size_t i = 4; i < a.size(); ++i) {
      if (a[i].compare(0, 3, "as=") == 0) continue;
      if (!line.empty()) line += " ";
      line += a[i];
    }
    if (line.empty()) fail(Status::Invalid, "a directive with nothing to do is not a directive");
    d.line = line;
    d.due_ms = now;                      // the first run is owed immediately
    need(reg.add(d));
    c.o << c.o.style(Green()) << "[DIRECTIVE]" << c.o.style(Reset()) << " " << d.name << " — every "
        << fmt_interval(d.every_ms) << ", as " << d.as.short_str() << "\n";
    c.o << "  " << d.line << "\n";
    c.o << "  it runs whenever this machine is idle; nothing has to be typed again\n";
    c.produced(d.name);
    return;
  }
  if (sub == "remove" || sub == "withdraw") {
    if (a.size() < 3) fail(Status::Invalid, "directive remove <name>");
    need(reg.remove(c.r.expand(a[2])));
    c.o << "withdrawn\n";
    return;
  }
  if (sub == "enable" || sub == "disable" || sub == "pause") {
    if (a.size() < 3) fail(Status::Invalid, "directive enable|disable <name>");
    need(reg.set_enabled(c.r.expand(a[2]), sub == "enable"));
    c.o << (sub == "enable" ? "standing again\n" : "paused\n");
    return;
  }
  if (sub == "run" || sub == "now") {
    // Force whatever is due (or one by name) to run this instant.
    if (a.size() > 2) {
      const auto* d = reg.find(c.r.expand(a[2]));
      if (!d) fail(Status::NotFound, "no standing directive is called " + c.r.expand(a[2]));
      userland::Directive copy = *d;
      copy.due_ms = 0;
      copy.enabled = true;
      need(reg.add(copy));
    }
    size_t ran = run_due_directives(c.s, true);
    c.o << ran << " directive(s) ran\n";
    return;
  }
  fail(Status::Invalid, "directive list | add <name> <every> as=$e <command…> | remove | enable | disable | run [name]");
}


// What this machine IS, in words it can hand to its mind: the kernel's identity,
// the statement it holds about itself (a `kind=identity` object, newest wins,
// else the architecture's own first principles), and the terms its own history
// says it works on. Every brief is anchored to this, not to a mind's guess.
static const char* kDefaultIdentity =
  "AIOS: a complete computing stack in one C++20 tree: its own kernel, hardware layers, storage, network, database, display and languages. "
  "A model can be bound to one slot; it is a component, not the system. "
  "State, compute, memory and communication over a substrate; entities holding capabilities under kernel authority; a resident mind that acts through the console as itself; "
  "zero-host: the kernel exists without Linux, boots on metal, composites its own display, and senses the world with its own TLS.";

static std::string identity_statement(Ctx& c, userland::Horizon* h) {
  std::string statement = kDefaultIdentity;
  std::vector<StoredObject> found;
  for (auto& id : c.k.store().find_meta("kind", "identity")) { auto o = c.k.store().get(id); if (o.ok() && !o->payload.empty()) found.push_back(*o); }
  if (!found.empty()) {
    std::sort(found.begin(), found.end(), [](const StoredObject& x, const StoredObject& y) { return x.timestamp_ns > y.timestamp_ns; });
    statement = std::string(found[0].payload.begin(), found[0].payload.end());
  }
  std::string out = "This machine is kernel " + c.k.self().short_str() + ".\nWhat it is: " + statement + "\n";
  if (h) {
    std::string terms; size_t n = 0;
    for (const auto& in : h->interests()) { if (n++ >= 18) break; if (!terms.empty()) terms += ", "; terms += in.term; }
    if (!terms.empty()) out += "What its own history says it works on: " + terms + "\n";
  }
  return out;
}

void cmd_identity(Ctx& c, const Args& a) {
  if (a.size() >= 3 && a[1] == "set") {
    need(c.me.operate());
    std::string text; for (size_t i = 2; i < a.size(); ++i) { if (!text.empty()) text += " "; text += a[i]; }
    StoredObject o;
    o.payload = ByteBuffer(text.begin(), text.end());
    o.embedding = embed_text(text);
    o.metadata.emplace_back("kind", "identity");
    o.metadata.emplace_back("author", c.me.self().str());
    ObjectID id = need(c.me.store(std::move(o)));
    c.o << "identity kept (object " << id.short_str() << ")\n";
    c.produced(id.str());
    return;
  }
  c.view();
  userland::Horizon h(c.me);
  (void)h.attach(false);
  c.o << c.o.style(Header()) << "Identity" << c.o.style(Reset()) << "\n";
  for (auto& line : split(identity_statement(c, &h), '\n')) if (!line.empty()) c.o << "  " << line << "\n";
  c.o << c.o.style(Dim()) << "  (identity set \"<what this machine is>\" to restate it)" << c.o.style(Reset()) << "\n";
}

void cmd_horizon(Ctx& c, const Args& a) {
  c.view();
  std::string sub = a.size() > 1 ? a[1] : "show";
  Surface actor = has_kv(a, "as") ? c.as(kv(a, "as")) : c.me;
  userland::Horizon h(actor);

  if (sub == "watch" || sub == "add") {
    need(h.attach());
    if (a.size() < 4) fail(Status::Invalid, "horizon watch <name> <url> — somewhere for this machine to look");
    need(h.add_source(c.r.expand(a[2]), c.r.expand(a[3])));
    c.o << c.o.style(Green()) << "[HORIZON]" << c.o.style(Reset()) << " watching " << c.r.expand(a[2]) << " — "
        << c.r.expand(a[3]) << "\n";
    c.produced(c.r.expand(a[2]));
    return;
  }
  if (sub == "unwatch" || sub == "remove") {
    need(h.attach());
    if (a.size() < 3) fail(Status::Invalid, "horizon unwatch <name>");
    need(h.remove_source(c.r.expand(a[2])));
    c.o << "no longer watched\n";
    return;
  }
  if (sub == "sources") {
    need(h.attach());
    if (h.sources().empty()) { c.o << "this machine watches nothing yet\n"; return; }
    c.o << c.o.style(Header()) << "Watched" << c.o.style(Reset()) << " (register state " << h.state().short_str()
        << (h.adopted() ? ", adopted from this kernel" : ", newly minted") << ")\n";
    c.o << "  NAME            READS  LAST\n";
    for (const auto& s : h.sources()) {
      c.o << "  " << W{16} << s.name << W{7} << s.reads << (s.last_ok ? "" : "! ") << s.last_result << "\n";
      c.o << "      " << c.o.style(Dim()) << s.url << c.o.style(Reset()) << "\n";
    }
    return;
  }
  if (sub == "interests" || sub == "cares") {
    auto in = h.interests();
    if (in.empty()) {
      c.o << "this machine has nothing of its own to measure the world against — no registered repository has "
             "commits in it yet\n";
      return;
    }
    c.o << c.o.style(Header()) << "What this machine cares about" << c.o.style(Reset())
        << ", worked out from its own repositories and the subjects of the commits written into them\n";
    size_t shown = 0;
    for (const auto& i : in) {
      if (shown++ >= 40) break;
      c.o << "  " << W{22} << i.term << Fixed{2} << W{8} << i.weight << W{6} << i.seen << " " << c.o.style(Dim()) << i.from
          << c.o.style(Reset()) << "\n";
    }
    c.o << "  " << in.size() << " term(s); nobody typed any of them\n";
    return;
  }
  if (sub == "scan" || sub == "look") {
    need(h.attach());
    size_t per = has_kv(a, "keep") ? (size_t)to_u64(kv(a, "keep")) : 6;
    auto rep = need(h.scan(per));
    c.o << c.o.style(Green()) << "[HORIZON]" << c.o.style(Reset()) << " read " << rep.sources_read << " source(s), "
        << rep.considered << " considered, " << rep.kept << " new, " << rep.already_known << " already known, in "
        << rep.took_ms << " ms\n";
    for (const auto& f : rep.failures) c.o << "  " << c.o.style(Red()) << "!" << c.o.style(Reset()) << " " << f << "\n";
    for (const auto& s : rep.signals) {
      c.o << "  " << (s.fresh ? "*" : " ") << " " << Fixed{2} << W{6} << s.score << " " << s.title << "\n";
      std::string terms;
      for (const auto& m : s.matched) { if (!terms.empty()) terms += ", "; terms += m; }
      c.o << "      " << c.o.style(Dim()) << s.source << " — touches " << terms << c.o.style(Reset()) << "\n";
    }
    if (rep.signals.empty())
      c.o << "  nothing out there touched what this machine is working on\n";
    return;
  }
  if (sub == "show" || sub == "list") {
    auto sig = h.signals(has_kv(a, "max") ? (size_t)to_u64(kv(a, "max")) : 20);
    if (sig.empty()) {
      c.o << "the horizon is empty — 'horizon watch <name> <url>' then 'horizon scan'\n";
      return;
    }
    c.o << c.o.style(Header()) << "The horizon" << c.o.style(Reset()) << " — " << sig.size()
        << " thing(s) the world did that touch this machine's own work\n";
    for (const auto& s : sig) {
      c.o << "  " << Fixed{2} << W{6} << s.score << s.title << "\n";
      std::string terms;
      for (const auto& m : s.matched) { if (!terms.empty()) terms += ", "; terms += m; }
      c.o << "      " << c.o.style(Dim()) << s.source << " — " << terms << c.o.style(Reset()) << "\n";
      c.o << "      " << c.o.style(Dim()) << s.url << c.o.style(Reset()) << "\n";
    }
    return;
  }
  if (sub == "distil" || sub == "distill") {
    // The other half of the horizon. The map above says WHAT the world did that
    // touches this machine's work; here the resident mind reads those things and
    // writes what they MEAN — one dated prose brief (kind=horizon-brief), kept in
    // the store like everything else it knows. On a fresh machine with no
    // history to measure against yet, it reads the watched sources' pages
    // directly (or, with none watched, a default few) — so day one is not
    // silence, and the brief sharpens as the machine gains history and sources.
    // A brief handed straight in — `horizon distil "<what it means>"` — IS the
    // brief: whoever is acting (the mind, when it has just perceived the world
    // itself) wrote it, and it is kept as-is. A mind that reached for this and
    // got a fresh re-derivation over the register's sources lost what it saw.
    {
      std::string given;
      for (size_t i = 2; i < a.size(); ++i) {
        const std::string& w = a[i];
        if (w.find("://") != std::string::npos && w.find(' ') == std::string::npos) continue;   // a feed to read, not the brief
        if (w.find('=') != std::string::npos && w.find(' ') == std::string::npos) continue;    // k=v
        if (!given.empty()) given += " ";
        given += w;
      }
      if (!given.empty()) {
        std::string date = has_kv(a, "date") ? kv(a, "date") : civil_date(c.k.host().wall_ns());
        StoredObject o;
        o.payload = ByteBuffer(given.begin(), given.end());
        o.embedding = embed_text(given);
        o.metadata.emplace_back("kind", "horizon-brief");
        o.metadata.emplace_back("date", date);
        o.metadata.emplace_back("sources", kv(a, "sources", "perceived"));
        o.metadata.emplace_back("author", actor.self().str());
        ObjectID id = need(actor.store(std::move(o)));
        c.o << c.o.style(Header()) << "Brief" << c.o.style(Reset()) << " · " << date << " · kept as this machine's horizon"
            << c.o.style(Dim()) << "  (object " << id.short_str() << ")" << c.o.style(Reset()) << "\n";
        c.produced(id.str());
        cmd_horizon(c, {"horizon", "brief"});   // show it the way it will be read
        return;
      }
    }
    std::string who = kv(a, "mind", "mind");
    size_t k = has_kv(a, "k") ? (size_t)to_u64(kv(a, "k")) : 3;
    if (k == 0) k = 3;
    size_t max_text = has_kv(a, "max") ? (size_t)to_u64(kv(a, "max")) : 7000;
    EntityID mind_id;
    for (auto& r : c.k.authority().residents()) if (r.label == who) mind_id = r.id;
    if (mind_id.is_null()) fail(Status::NotFound, "no resident mind called '" + who + "'");
    std::string date = civil_date(c.k.host().wall_ns());
    (void)h.attach(false);   // a reader: never mint a register just to look
    const std::string identity = identity_statement(c, &h);
    auto squash = [](std::string t) { for (auto& ch : t) if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' '; std::string o; bool sp = false; for (char ch : t) { if (ch == ' ') { if (!sp) o += ' '; sp = true; } else { o += ch; sp = false; } } while (!o.empty() && o.front() == ' ') o.erase(o.begin()); while (!o.empty() && o.back() == ' ') o.pop_back(); return o; };
    auto same_url = [](std::string x, std::string y) { while (!x.empty() && x.back() == '/') x.pop_back(); while (!y.empty() && y.back() == '/') y.pop_back(); return x == y; };

    // 1. The front pages this machine watches, read with links, so the mind
    //    chooses from what is really there — never from a URL it invents.
    std::vector<std::string> feeds;
    for (const auto& src : h.sources()) feeds.push_back(src.url);
    for (size_t i = 2; i < a.size(); ++i) if (a[i].find("://") != std::string::npos && a[i].find(' ') == std::string::npos) feeds.push_back(c.r.expand(a[i]));
    if (feeds.empty()) feeds = {"https://news.ycombinator.com", "https://lobste.rs", "https://www.cisa.gov/news-events/cybersecurity-advisories"};
    c.o << c.o.style(Header()) << "Horizon distil" << c.o.style(Reset()) << " · " << date << " · reading " << feeds.size() << " front page(s) for what the world did\n";
    c.o.flush();
    struct Cand { std::string url, text, from; };
    std::vector<Cand> cands;
    std::string front;
    userland::SenseOptions fopt; fopt.embed = &embed_text; fopt.max_text = 3000; fopt.keep = false; fopt.collect_links = true;
    for (const auto& u : feeds) {
      userland::Sense sense(c.k, &actor);
      auto p = sense.look(u, fopt);
      if (!p.ok()) { c.o << "  " << c.o.style(Red()) << "x" << c.o.style(Reset()) << " " << u << " — " << p.error().str() << "\n"; c.o.flush(); continue; }
      size_t n = 0;
      std::string page = "\n=== " + (p->title.empty() ? u : p->title) + " (" + u + ") ===\n";
      for (const auto& l : p->links) {
        if (l.url.find("://") == std::string::npos) continue;
        std::string t = squash(l.text);
        if (t.size() < 16) continue;   // "hide", "12 comments", "login": navigation, not news
        bool dup = false; for (auto& cd : cands) if (same_url(cd.url, l.url)) { dup = true; break; }
        if (dup) continue;
        cands.push_back({l.url, t, p->title});
        page += "  " + t + "\n    " + l.url + "\n";
        if (++n >= 80) break;
      }
      front += page;
      c.o << "  " << c.o.style(Green()) << "+" << c.o.style(Reset()) << " " << (p->title.empty() ? u : p->title) << c.o.style(Dim()) << "  (" << n << " headlines, " << fmt_ns(p->took_ns) << ")" << c.o.style(Reset()) << "\n";
      c.o.flush();
    }
    if (cands.empty()) fail(Status::IoError, "horizon distil: no front page could be read, or none linked anywhere");

    // 2. The mind chooses which to open — from those links only.
    auto urls_in = [](const std::string& text) {
      std::vector<std::string> out; size_t at = 0;
      while ((at = text.find("http", at)) != std::string::npos) {
        size_t end = at;
        // no isspace: libc's ctype is a hosted symbol the bare-metal image cannot link
        auto space = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v'; };
        while (end < text.size() && !space(text[end]) && text[end] != '"' && text[end] != '\'' && text[end] != ')' && text[end] != '>' && text[end] != ']' && text[end] != '`') ++end;
        std::string u = text.substr(at, end - at);
        while (!u.empty() && (u.back() == '.' || u.back() == ',' || u.back() == ';' || u.back() == '*')) u.pop_back();
        if (u.size() > 8) out.push_back(u);
        at = end;
      }
      return out;
    };
    const size_t want = 2 * k;
    std::string pick_req =
      "You are perceiving the world for " + date + " on behalf of this machine.\n" + identity +
      "Below are the front pages this machine watches, as headline then URL. Choose the " + std::to_string(want) + " headlines most likely to be genuine engineering breakthroughs or systemic shifts that touch what this machine is and does — "
      "not personal essays, jobs, ads, navigation, or discussion threads about an article (choose the article itself).\n"
      "Reply directly, without running commands:\n    answer:\n    <URL>\n    <URL>\n    ...\n"
      "Copy each URL exactly from the list; a URL that is not in the list is discarded.\n" + front;
    std::string picked;
    need(ask(c.s, pick_req, who, 3, &picked));
    std::vector<std::string> chosen;
    for (auto& u : urls_in(picked)) {
      const Cand* hit = nullptr; for (auto& cd : cands) if (same_url(cd.url, u)) { hit = &cd; break; }
      if (!hit) continue;
      bool dup = false; for (auto& ch : chosen) if (same_url(ch, hit->url)) { dup = true; break; }
      if (!dup) chosen.push_back(hit->url);
      if (chosen.size() >= want) break;
    }
    if (chosen.empty()) {
      c.o << "  " << c.o.style(Dim()) << "the mind named nothing that was on the pages; opening the first " << want << " headlines instead" << c.o.style(Reset()) << "\n";
      for (auto& cd : cands) { chosen.push_back(cd.url); if (chosen.size() >= want) break; }
    }

    // 3. Open them — the articles themselves, kept in the store.
    struct Read { std::string url, title, text; };
    std::vector<Read> reads;
    userland::SenseOptions ropt; ropt.embed = &embed_text; ropt.max_text = max_text; ropt.keep = true;
    c.o << "  opening " << chosen.size() << " article(s)\n"; c.o.flush();
    for (const auto& u : chosen) {
      userland::Sense sense(c.k, &actor);
      auto p = sense.look(u, ropt);
      if (!p.ok()) { c.o << "  " << c.o.style(Red()) << "x" << c.o.style(Reset()) << " " << u << " — " << p.error().str() << "\n"; c.o.flush(); continue; }
      reads.push_back({u, p->title.empty() ? u : p->title, p->text});
      c.o << "  " << c.o.style(Green()) << "+" << c.o.style(Reset()) << " " << reads.back().title << c.o.style(Dim()) << "  (" << fmt_bytes(p->text_bytes) << ", " << fmt_ns(p->took_ns) << ")" << c.o.style(Reset()) << "\n";
      c.o.flush();
    }
    if (reads.empty()) fail(Status::IoError, "horizon distil: none of the chosen articles could be read");

    // 4. The brief — every item tied to something that was actually read.
    struct Item { std::string title, what, why, url; };
    auto brief_req = [&](const std::string& correction) {
      std::string r =
        "Horizon brief for " + date + ".\n" + identity +
        "You have just read the " + std::to_string(reads.size()) + " article(s) below with this machine's own eyes.\n"
        "Write the machine's brief: exactly " + std::to_string(k) + " items — the most significant engineering breakthroughs or systemic shifts — each anchored to what THIS machine is and does (its identity above), not to generic relevance.\n"
        "One item per line, four fields separated by ' :: ':\n"
        "    TITLE :: what happened, concretely, with names and numbers :: why it touches this machine :: URL\n"
        "URL must be one of the article URLs below, copied exactly. An item citing anything you did not read is discarded.\n"
        "Reply:\n    answer:\n    <item>\n    <item>\n    <item>\n"
        "(You may first `run: store similar <topic>` to check what this machine already remembers.)\n" + correction;
      for (const auto& rd : reads) {
        std::string t = rd.text; if (t.size() > max_text) t = t.substr(0, max_text) + "…";
        r += "\n=== " + rd.title + " (" + rd.url + ") ===\n" + t + "\n";
      }
      return r;
    };
    auto parse = [&](const std::string& text, std::vector<Item>& good, std::vector<std::string>& bad) {
      for (auto& raw : split(text, '\n')) {
        std::string line = squash(raw);
        if (line.rfind("answer:", 0) == 0) line = squash(line.substr(7));
        if (line.empty() || line.find(" :: ") == std::string::npos) continue;
        std::vector<std::string> f; size_t at = 0;
        while (true) { size_t sep = line.find(" :: ", at); f.push_back(squash(line.substr(at, sep == std::string::npos ? std::string::npos : sep - at))); if (sep == std::string::npos) break; at = sep + 4; }
        if (f.size() < 4) { bad.push_back("not four fields: " + line.substr(0, 80)); continue; }
        Item it{f[0], f[1], f[2], f[3]};
        auto us = urls_in(it.url); if (!us.empty()) it.url = us[0];
        while (!it.title.empty() && (it.title.front() == '-' || it.title.front() == '*' || it.title.front() == ' ' || isdigit((unsigned char)it.title.front()) || it.title.front() == '.')) it.title.erase(it.title.begin());
        bool read_it = false; for (auto& rd : reads) if (same_url(rd.url, it.url)) { it.url = rd.url; read_it = true; break; }
        if (!read_it) { bad.push_back(it.url + " — not one of the articles you read"); continue; }
        good.push_back(it);
      }
    };
    std::string ans;
    need(ask(c.s, brief_req(""), who, 4, &ans));
    std::vector<Item> items; std::vector<std::string> bad; parse(ans, items, bad);
    if (items.size() < k) {
      std::string corr = "\nYour previous reply was not accepted:\n";
      for (auto& b : bad) corr += "  - " + b + "\n";
      corr += "  - " + std::to_string(items.size()) + " valid item(s); exactly " + std::to_string(k) + " are required, each citing an article URL from below.\nReply again, in the format above.\n";
      c.o << "  " << c.o.style(Dim()) << "brief not accepted (" << items.size() << " of " << k << " items grounded); asking once more" << c.o.style(Reset()) << "\n"; c.o.flush();
      items.clear(); bad.clear();
      need(ask(c.s, brief_req(corr), who, 4, &ans));
      parse(ans, items, bad);
    }
    if (items.empty()) fail(Status::Corrupt, "horizon distil: the mind's brief cited nothing it had read");
    if (items.size() > k) items.resize(k);

    // 5. Keep it as the machine's horizon, written by the mind.
    std::string payload, urls;
    for (auto& it : items) {
      if (!payload.empty()) payload += " | ";
      payload += it.title + " :: " + it.what + " :: " + it.why + " :: " + it.url;
      if (!urls.empty()) urls += ",";
      urls += it.url;
    }
    StoredObject o;
    o.payload = ByteBuffer(payload.begin(), payload.end());
    o.embedding = embed_text(payload);
    o.metadata.emplace_back("kind", "horizon-brief");
    o.metadata.emplace_back("date", date);
    o.metadata.emplace_back("sources", std::to_string(reads.size()));
    o.metadata.emplace_back("urls", urls);
    o.metadata.emplace_back("author", mind_id.str());
    auto as_mind = c.me.act_as(mind_id);
    ObjectID id = need((as_mind.ok() ? *as_mind : actor).store(std::move(o)));
    c.o << c.o.style(Header()) << "Brief" << c.o.style(Reset()) << " · " << date << " · " << items.size() << " item(s) from " << reads.size() << " article(s) read"
        << c.o.style(Dim()) << "  (object " << id.short_str() << ")" << c.o.style(Reset()) << "\n";
    for (auto& it : items) {
      c.o << "  - " << c.o.style(Bold()) << it.title << c.o.style(Reset()) << "\n";
      c.o << "      " << it.what << "\n";
      c.o << "      " << c.o.style(Cyan()) << "why us: " << c.o.style(Reset()) << it.why << "\n";
      c.o << "      " << c.o.style(Dim()) << it.url << c.o.style(Reset()) << "\n";
    }
    if (items.size() < k) c.o << "  " << c.o.style(Dim()) << "(" << (k - items.size()) << " item(s) were dropped for citing nothing that was read)" << c.o.style(Reset()) << "\n";
    c.produced(id.str());
    return;
  }
  if (sub == "brief") {
    // The latest brief(s), newest first, one item per line.
    size_t n = 1;
    for (size_t i = 2; i < a.size(); ++i) { uint64_t v; if (parse_u64(a[i], v) && v > 0) n = (size_t)v; }
    std::vector<StoredObject> found;
    for (auto& id : c.k.store().find_meta("kind", "horizon-brief")) { auto o = c.k.store().get(id); if (o.ok()) found.push_back(*o); }
    if (found.empty()) { c.o << "no brief yet — run: horizon distil   (give it standing: directive add brief 1d as=$mind horizon distil)\n"; return; }
    std::sort(found.begin(), found.end(), [](const StoredObject& x, const StoredObject& y) { return x.timestamp_ns > y.timestamp_ns; });
    if (found.size() > n) found.resize(n);
    for (auto& o : found) {
      std::string date, sources;
      for (auto& [k, v] : o.metadata) { if (k == "date") date = v; else if (k == "sources") sources = v; }
      c.o << c.o.style(Header()) << "Brief" << c.o.style(Reset()) << " · " << (date.empty() ? fmt_u64(o.timestamp_ns / 1000000000ull) + "s" : date)
          << (sources.empty() ? "" : " · " + sources + " sources") << c.o.style(Dim()) << "  (object " << o.id.short_str() << ")" << c.o.style(Reset()) << "\n";
      std::string text(o.payload.begin(), o.payload.end());
      size_t at = 0;
      while (at <= text.size()) {
        size_t bar = text.find(" | ", at);
        std::string item = text.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
        while (!item.empty() && item.front() == ' ') item.erase(item.begin());
        if (!item.empty()) {
          // A distilled item is "TITLE :: what :: why :: URL"; older briefs are prose.
          std::vector<std::string> f; size_t p0 = 0;
          while (true) { size_t sep = item.find(" :: ", p0); f.push_back(item.substr(p0, sep == std::string::npos ? std::string::npos : sep - p0)); if (sep == std::string::npos) break; p0 = sep + 4; }
          if (f.size() >= 4) {
            c.o << "  - " << c.o.style(Bold()) << f[0] << c.o.style(Reset()) << "\n      " << f[1] << "\n      " << c.o.style(Cyan()) << "why us: " << c.o.style(Reset()) << f[2] << "\n      " << c.o.style(Dim()) << f[3] << c.o.style(Reset()) << "\n";
          } else {
            c.o << "  - " << item << "\n";
          }
        }
        if (bar == std::string::npos) break;
        at = bar + 3;
      }
    }
    c.produced(found[0].id.str());
    return;
  }
  fail(Status::Invalid, "horizon show | scan | distil | brief | sources | watch <name> <url> | unwatch <name> | interests");
}

void cmd_elf(Ctx& c, const Args& a) {
  c.view();
  if (a.size() < 2) fail(Status::Invalid, "elf <path> — what this machine would have to do to run that file");
  Surface actor = has_kv(a, "as") ? c.as(kv(a, "as")) : c.me;
  auto f = need(userland::elf_open(c.k, &actor, c.r.expand(a[1])));
  const auto& i = f.image;
  c.o << c.o.style(Header()) << f.path << c.o.style(Reset()) << "\n";
  c.o << "  " << W{14} << "kind" << ": " << i.kind << ", " << i.machine << ", "
      << (i.static_linked ? "statically linked" : "needs the interpreter " + i.interpreter) << "\n";
  c.o << "  " << W{14} << "entry" << ": " << fmt_hex(i.entry) << "\n";
  c.o << "  " << W{14} << "wants" << ": " << fmt_hex(i.lowest) << " … " << fmt_hex(i.highest) << "  ("
      << fmt_bytes(i.highest - i.lowest) << " of address space, from " << fmt_bytes(i.bytes) << " of file)\n";
  for (const auto& s : i.segments)
    c.o << "  " << W{14} << ("  " + s.rights()) << ": " << fmt_hex(s.vaddr) << "  " << W{10} << fmt_bytes(s.file_size)
        << " on disk, " << fmt_bytes(s.mem_size) << " in memory\n";
  c.produced(fmt_hex(i.entry));
}

void cmd_go(Ctx& c, const Args& a) {
  c.view();
  Surface actor = has_kv(a, "as") ? c.as(kv(a, "as")) : c.me;
  userland::GoOptions opt;
  if (has_kv(a, "proxy")) opt.proxy = c.r.expand(kv(a, "proxy"));
  if (has_kv(a, "sumdb")) opt.sumdb = c.r.expand(kv(a, "sumdb"));
  if (has_kv(a, "key")) opt.sumdb_key = c.r.expand(kv(a, "key"));
  if (has_kv(a, "timeout")) opt.timeout_ms = (int)to_u64(kv(a, "timeout"));
  if (has_kv(a, "deps")) opt.with_dependencies = kv(a, "deps") != "0" && kv(a, "deps") != "no";
  if (has_kv(a, "sums")) opt.verify_sums = kv(a, "sums") != "0" && kv(a, "sums") != "no";
  userland::GoModules go(c.k, &actor);

  // "go mod download" and "go download" mean the same thing here; the word mod
  // is where the real command keeps them, so it is accepted and stepped over.
  std::vector<std::string> rest;
  for (size_t i = 1; i < a.size(); ++i) {
    if (i == 1 && a[i] == "mod") continue;
    rest.push_back(a[i]);
  }
  std::string sub = rest.empty() ? "help" : rest[0];
  auto operand = [&]() -> std::string {
    for (size_t i = 1; i < rest.size(); ++i)
      if (rest[i].find('=') == std::string::npos) return c.r.expand(rest[i]);
    return {};
  };
  auto where = [&]() -> std::string {
    if (has_kv(a, "dir")) return c.r.expand(kv(a, "dir"));
    userland::GitRegistry reg(actor);
    if (reg.attach().ok()) { auto cur = reg.current(); if (cur.ok()) return *cur; }
    return c.k.config().root;
  };

  if (sub == "list" || sub == "view" || sub == "versions") {
    std::string path = operand();
    if (path.empty()) fail(Status::Invalid, "go list <module path>");
    std::string latest;
    auto vs = need(go.versions(path, latest, opt));
    c.o << c.o.style(Header()) << path << " — " << vs.size() << " version(s), latest "
        << c.o.style(Cyan()) << latest << c.o.style(Reset()) << "\n";
    for (size_t i = vs.size(); i > 0 && i + 8 > vs.size(); --i) c.o << "  " << vs[i - 1] << "\n";
    c.produced(latest);
    return;
  }
  if (sub == "download" || sub == "get" || sub == "vendor") {
    std::string spec = operand();
    if (spec.empty()) fail(Status::Invalid, "go get <module>[@version] [dir=<path>]");
    auto rep = need(go.download(where(), spec, opt));
    c.o << c.o.style(Green()) << "[GO DOWNLOAD]" << c.o.style(Reset()) << " " << spec << "\n";
    c.o << "  " << W{14} << "proxy" << ": " << rep.proxy << " over " << rep.tls << "\n";
    c.o << "  " << W{14} << "trusted via" << ": " << rep.trust_anchor << "\n";
    if (opt.verify_sums) {
      c.o << "  " << W{14} << "checksum db" << ": " << rep.sumdb << " — " << rep.tree.origin << " of "
          << fmt_u64((uint64_t)rep.tree.size) << " records, signed by " << rep.tree.signer << "\n";
      c.o << "  " << W{14} << "proved into" << ": " << rep.tree.root << " (" << rep.tree.tiles_read
          << " log tiles read to prove it)\n";
    } else {
      c.o << "  " << W{14} << "checksum db" << ": " << rep.sumdb << "\n";
    }
    c.o << "  " << W{14} << "into" << ": " << rep.target << "\n";
    for (auto& m : rep.modules)
      c.o << "  " << W{34} << m.path << W{38} << m.version << W{10} << (fmt_u64(m.files) + " files")
          << W{10} << fmt_bytes(m.bytes) << "  " << (m.sum_verified ? m.zip_hash : std::string("(unverified)")) << "\n";
    c.o << "  " << W{14} << "total" << ": " << rep.modules.size() << " module(s), " << fmt_bytes(rep.downloaded_bytes)
        << " in " << fmt_ns(rep.took_ns) << " — go.sum written beside the vendor tree\n";
    if (!rep.modules.empty()) c.produced(rep.modules.front().version);
    return;
  }
  if (sub == "verify") {
    auto rep = need(go.verify(where(), opt));
    c.o << c.o.style(Green()) << "[GO VERIFY]" << c.o.style(Reset()) << " " << rep.target << "\n";
    for (auto& m : rep.modules)
      c.o << "  " << W{34} << m.path << W{38} << m.version << W{10} << (fmt_u64(m.files) + " files") << "  "
          << m.zip_hash << "\n";
    c.o << "  " << W{14} << "total" << ": " << rep.modules.size()
        << " module(s) still hash to what go.sum recorded, in " << fmt_ns(rep.took_ns) << "\n";
    c.produced(fmt_u64(rep.modules.size()));
    return;
  }
  fail(Status::Invalid,
       "go get <module>[@version] [dir=<path>] [deps=0] [sums=0] | go list <module> | go mod verify [dir=<path>]");
}

void cmd_git(Ctx& c, const Args& a) {
  c.view();
  std::string sub = (a.size() > 1) ? a[1] : "status";
  // git acts AS an entity, like everything else on this machine: `as=$e` runs
  // the subsystem under that entity's authority and the kernel refuses what it
  // may not do. Without it, the console's own authority is what is spent.
  Surface actor = has_kv(a, "as") ? c.as(kv(a, "as")) : c.me;
  userland::GitRegistry reg(actor);
  need(reg.attach());
  userland::Git git(c.k, &actor);
  const Entity* self = c.k.entities().get(actor.self());
  git.set_author(self && !self->label.empty() ? self->label : "aios", "console@aios");
  auto remember = [&](bool make_current) {
    userland::GitRepoRecord rec;
    rec.path = git.work_dir();
    size_t slash = rec.path.rfind('/');
    rec.name = slash == std::string::npos ? rec.path : rec.path.substr(slash + 1);
    rec.branch = git.branch();
    auto h = git.head_oid();
    if (h.ok()) rec.head = *h;
    rec.registered_ns = c.k.now();
    const userland::GitRepoRecord* prev = reg.find(rec.path);
    rec.commits = prev ? prev->commits : 0;
    need(reg.record(rec, make_current));
  };

  if (sub == "help" || sub == "--help" || sub == "-h") {
    c.o << c.o.style(Header()) << "AIOS Git Subsystem" << c.o.style(Reset()) << "\n\n"
        << "Repository Management:\n"
        << "  git init <name|dir>             Initialize and register a repository\n"
        << "  git use <name|dir>              Switch active repository context\n"
        << "  git repos                       List registered repositories\n"
        << "  git fsck                        Verify object database integrity\n\n"
        << "Working Tree & History:\n"
        << "  git status                      Show working tree and index status\n"
        << "  git add <path...>               Stage file contents for commit\n"
        << "  git commit -m <msg>             Record changes to repository\n"
        << "  git log [-n N]                  Show commit history\n"
        << "  git diff [path]                 Show changes between commits/working tree\n"
        << "  git branch [name]               List or create branches\n"
        << "  git switch <branch>             Switch branches\n\n"
        << "Remote Operations:\n"
        << "  git clone <url> [dir]           Clone repository over ssh:// or git://\n"
        << "  git fetch [url]                 Fetch refs and objects from remote\n"
        << "  git pull [url]                  Fetch and merge from remote\n"
        << "  git push [url] [branch]         Push local changes to remote\n"
        << "  git ls-remote <url>             List remote references\n";
    return;
  }

  if (sub == "init") {
    if (a.size() < 3) fail(Status::Invalid, "git init <name|dir>");
    // A bare name is a repository OF THIS MACHINE, under the kernel root.
    std::string dir = userland::git_repo_path(c.k, c.r.expand(a[2]));
    need(git.init(dir));
    remember(true);
    c.produced(git.work_dir());
    c.o << c.o.style(Green()) << "[GIT INIT]" << c.o.style(Reset()) << " object database at " << c.o.style(Cyan()) << git.git_dir() << c.o.style(Reset())
        << " on branch " << git.branch() << " (loose objects, SHA-1 ids, zlib streams — written by the kernel itself)\n";
    c.o << "  registered with the kernel as " << c.o.style(Cyan()) << reg.state().short_str() << c.o.style(Reset())
        << ", held by entity " << reg.holder().short_str() << " — a checkpoint of it carries this repository\n";
    return;
  }

  // `switch` means what git means by it — moving between branches — so the
  // repository selector keeps `use` and `cd` and gives that word up.
  if (sub == "use" || sub == "cd" || sub == "open") {
    if (a.size() < 3) fail(Status::Invalid, "git use <name|dir>");
    std::string want = c.r.expand(a[2]);
    const userland::GitRepoRecord* known = reg.find(want);
    std::string dir = known ? known->path : userland::git_repo_path(c.k, want);
    need(git.open(dir));
    remember(true);
    c.o << c.o.style(Green()) << "[GIT USE]" << c.o.style(Reset()) << " " << git.work_dir()
        << " on branch " << (git.branch().empty() ? "(detached HEAD)" : git.branch()) << "\n";
    return;
  }

  if (sub == "repos" || sub == "list") {
    // The subsystem's own registry: what this KERNEL knows, not what a
    // directory scan would find.
    auto cur = reg.current();
    c.o << c.o.style(Header()) << "Git repositories registered with kernel " << c.k.self().short_str()
        << " (state " << reg.state().short_str() << ", held by " << reg.holder().short_str()
        << (reg.adopted() ? ", adopted — it was already in this kernel" : ", created in this process") << ")" << c.o.style(Reset()) << "\n";
    if (reg.repos().empty()) c.o << "  (none — `git init <name>` registers one)\n";
    for (auto& r : reg.repos()) {
      c.o << "  " << (cur.ok() && *cur == r.path ? "* " : "  ") << W{16} << r.name << W{12} << (r.branch.empty() ? "(detached)" : r.branch)
          << " " << c.o.style(Cyan()) << (r.head.empty() ? std::string("(unborn branch)") : r.head) << c.o.style(Reset())
          << "  " << r.commits << " commit(s) here  " << r.path << "\n";
    }
    return;
  }

  // ---- talking to a remote: the wire protocol, spoken by this kernel ----
  auto remote_options = [&](userland::GitRemoteOptions& opt) {
    opt.ssh_key = has_kv(a, "key") ? c.r.expand(kv(a, "key")) : ssh_key_path(c.k);
    opt.known_hosts = ssh_known_hosts(c.k);
    if (has_kv(a, "depth")) opt.depth = (int)to_u64(kv(a, "depth"));
    if (has_kv(a, "ref")) opt.ref = c.r.expand(kv(a, "ref"));
    if (has_kv(a, "timeout")) opt.timeout_ms = (int)to_u64(kv(a, "timeout"));
    if (has_kv(a, "checkout")) opt.checkout = kv(a, "checkout") != "0" && kv(a, "checkout") != "no";
  };
  auto show_transport = [&](const userland::GitFetchReport& rep) {
    if (rep.transport == "ssh") {
      c.o << "  " << W{14} << "transport" << ": ssh to " << rep.user << "@" << rep.host << ":" << rep.port
          << " — " << rep.kex << " + " << rep.cipher << "\n";
      c.o << "  " << W{14} << "host key" << ": " << rep.host_key_fingerprint << "  (" << rep.server_version << ")\n";
    } else if (rep.transport == "https" || rep.transport == "http") {
      c.o << "  " << W{14} << "transport" << ": smart HTTP to " << rep.host << ":" << rep.port;
      if (rep.transport == "https") c.o << " over " << rep.kex << " " << rep.cipher;
      c.o << "\n";
      if (rep.transport == "https")
        c.o << "  " << W{14} << "certificate" << ": " << rep.server_version << " (trusted via " << rep.host_key_fingerprint << ")\n";
    } else {
      c.o << "  " << W{14} << "transport" << ": git daemon at " << rep.host << ":" << rep.port << "\n";
    }
  };

  if (sub == "ls-remote" || sub == "remote-refs") {
    if (a.size() < 3) fail(Status::Invalid, "git ls-remote <url> [key=<path>]");
    need_network(actor);
    userland::GitRemoteOptions opt;
    remote_options(opt);
    userland::GitRemote remote(c.k, &actor);
    auto rep = need(remote.ls_remote(c.r.expand(a[2]), opt));
    c.o << c.o.style(Header()) << "Refs on " << rep.url << " (" << rep.refs.size() << ")" << c.o.style(Reset()) << "\n";
    show_transport(rep);
    if (!rep.default_branch.empty()) c.o << "  " << W{14} << "HEAD" << ": " << rep.default_branch << "\n";
    for (auto& r : rep.refs) c.o << "  " << c.o.style(Cyan()) << r.oid << c.o.style(Reset()) << "  " << r.name << "\n";
    if (!rep.refs.empty()) c.produced(rep.refs.front().oid);
    return;
  }

  if (sub == "clone") {
    if (a.size() < 3) fail(Status::Invalid, "git clone <url> [name|dir] [key=<path>] [depth=N] [ref=<name>]");
    need_network(actor);
    std::string url = c.r.expand(a[2]);
    std::string name;
    for (size_t i = 3; i < a.size(); ++i)
      if (a[i].find('=') == std::string::npos) { name = c.r.expand(a[i]); break; }
    if (name.empty()) {
      auto u = need(userland::git_parse_url(url));
      std::string path = u.path;
      while (!path.empty() && path.back() == '/') path.pop_back();
      size_t slash = path.rfind('/');
      name = slash == std::string::npos ? path : path.substr(slash + 1);
      if (name.size() > 4 && name.compare(name.size() - 4, 4, ".git") == 0) name = name.substr(0, name.size() - 4);
      if (name.empty()) fail(Status::Invalid, "cannot tell what to call the clone; name it: git clone <url> <name>");
    }
    std::string dir = userland::git_repo_path(c.k, name);
    {
      userland::Git probe(c.k, &actor);
      if (probe.open(dir).ok())
        fail(Status::Conflict, dir + " already holds a repository — `git use " + name + "` opens it");
    }

    userland::GitRemoteOptions opt;
    remote_options(opt);
    userland::GitRemote remote(c.k, &actor);
    uint64_t t0 = c.k.host().monotonic_ns();
    auto rep = need(remote.clone(url, dir, opt));
    uint64_t took = c.k.host().monotonic_ns() - t0;

    need(git.open(dir));
    // What real git writes, so real git can carry on from here.
    need(git.set_remote("origin", url));
    remember(true);
    c.produced(rep.head_oid);
    c.o << c.o.style(Green()) << "[GIT CLONE]" << c.o.style(Reset()) << " " << url << " into " << c.o.style(Cyan()) << dir
        << c.o.style(Reset()) << "\n";
    show_transport(rep);
    c.o << "  " << W{14} << "pack" << ": " << fmt_bytes(rep.pack_bytes) << ", " << rep.objects
        << " object(s) — " << rep.deltas << " delta-encoded, all reconstructed here\n";
    c.o << "  " << W{14} << "database" << ": " << rep.written << " object(s) written loose into " << git.git_dir() << "/objects\n";
    c.o << "  " << W{14} << "branch" << ": " << rep.default_branch << " @ " << rep.head_oid << "\n";
    c.o << "  " << W{14} << "work tree" << ": " << rep.files << " file(s) checked out\n";
    c.o << "  " << W{14} << "took" << ": " << fmt_ns(took) << "\n";
    c.o << "  registered with the kernel as " << c.o.style(Cyan()) << reg.state().short_str() << c.o.style(Reset())
        << " — a checkpoint of this machine carries it\n";
    return;
  }

  std::string where = need(reg.current());
  need(git.open(where));

  if (sub == "branch" || sub == "branches") {
    if (a.size() > 2 && a[2].find('=') == std::string::npos && a[2] != "-d") {
      std::string name = c.r.expand(a[2]);
      std::string at = (a.size() > 3 && a[3].find('=') == std::string::npos) ? c.r.expand(a[3]) : std::string();
      need(git.create_branch(name, at));
      auto tip = need(git.ref_oid("refs/heads/" + name));
      c.produced(tip);
      c.o << c.o.style(Green()) << "[BRANCH]" << c.o.style(Reset()) << " " << name << " at " << c.o.style(Cyan()) << tip
          << c.o.style(Reset()) << " — `git switch " << name << "` moves onto it\n";
      return;
    }
    if (a.size() > 3 && a[2] == "-d") {
      std::string name = c.r.expand(a[3]);
      need(git.delete_branch(name));
      c.o << c.o.style(Green()) << "[BRANCH]" << c.o.style(Reset()) << " deleted " << name << "\n";
      return;
    }
    auto list = need(git.branches());
    c.o << c.o.style(Header()) << "Branches of " << git.work_dir() << c.o.style(Reset()) << "\n";
    if (list.empty()) c.o << "  (none yet — the branch is unborn until the first commit)\n";
    for (auto& b : list)
      c.o << "  " << (b.name == git.branch() ? "* " : "  ") << W{20} << b.name << c.o.style(Cyan()) << b.oid
          << c.o.style(Reset()) << "\n";
    if (git.branch().empty()) c.o << "  HEAD is detached at " << need(git.head_oid()) << "\n";
    return;
  }

  if (sub == "switch" || sub == "checkout" || sub == "co") {
    if (a.size() < 3) fail(Status::Invalid, "git switch <branch|commit> [discard=1]");
    std::string rev = c.r.expand(a[2]);
    bool discard = has_kv(a, "discard") && kv(a, "discard") != "0" && kv(a, "discard") != "no";
    auto files = need(git.switch_to(rev, discard));
    auto at = need(git.head_oid());
    c.produced(at);
    c.o << c.o.style(Green()) << "[SWITCH]" << c.o.style(Reset()) << " "
        << (git.branch().empty() ? "detached at " + at : "on " + git.branch() + " (" + at + ")") << "\n";
    c.o << "  " << W{14} << "work tree" << ": " << files << " file(s) laid down to match that commit\n";
    return;
  }

  if (sub == "push") {
    need_network(actor);
    std::string url, ref;
    for (size_t i = 2; i < a.size(); ++i) {
      if (a[i].find('=') != std::string::npos) continue;
      if (url.empty() && (a[i].find("://") != std::string::npos || a[i].find('@') != std::string::npos)) { url = c.r.expand(a[i]); continue; }
      if (ref.empty()) ref = c.r.expand(a[i]);
    }
    if (url.empty()) url = need(git.remote_url("origin"));
    userland::GitRemoteOptions opt;
    remote_options(opt);
    bool force = has_kv(a, "force") && kv(a, "force") != "0" && kv(a, "force") != "no";
    userland::GitRemote remote(c.k, &actor);
    auto rep = need(remote.push(git, url, ref, force, opt));
    c.o << c.o.style(Green()) << "[GIT PUSH]" << c.o.style(Reset()) << " " << git.work_dir() << " -> " << url << "\n";
    {
      userland::GitFetchReport t;
      t.transport = rep.transport; t.host = rep.host; t.port = rep.port; t.user = rep.user;
      t.kex = rep.kex; t.cipher = rep.cipher;
      t.host_key_fingerprint = rep.host_key_fingerprint; t.server_version = rep.server_version;
      show_transport(t);
    }
    if (rep.up_to_date) {
      c.o << "  " << W{14} << "ref" << ": " << rep.ref << " is already at " << c.o.style(Cyan()) << rep.local_oid
          << c.o.style(Reset()) << " on the remote — nothing to send\n";
      c.produced(rep.local_oid);
      return;
    }
    c.o << "  " << W{14} << "pack" << ": " << fmt_bytes(rep.pack_bytes) << ", " << rep.objects
        << " object(s) written and compressed by this kernel\n";
    c.o << "  " << W{14} << "ref" << ": " << rep.ref << "  "
        << (rep.created ? std::string("(created)") : rep.remote_old.substr(0, 12) + " -> " + rep.local_oid.substr(0, 12))
        << (rep.forced ? "  (forced — not a fast-forward)" : "") << "\n";
    c.o << "  " << W{14} << "remote says" << ": unpack " << rep.unpack_status << ", " << rep.ref << " " << rep.ref_status << "\n";
    c.produced(rep.local_oid);
    return;
  }

  if (sub == "fetch" || sub == "pull") {
    need_network(actor);
    bool pulling = (sub == "pull");
    std::string url;
    for (size_t i = 2; i < a.size(); ++i)
      if (a[i].find('=') == std::string::npos) { url = c.r.expand(a[i]); break; }
    // The remote this repository was cloned from, as the subsystem recorded it.
    if (url.empty()) url = need(git.remote_url("origin"));
    userland::GitRemoteOptions opt;
    remote_options(opt);
    // A fetch brings the remote's refs down and touches nothing of this
    // machine's; a pull moves the branch it is standing on, and the files with it.
    opt.advance_branches = pulling;
    opt.checkout = pulling;
    userland::GitRemote remote(c.k, &actor);
    auto rep = need(remote.fetch(git, url, opt));
    c.o << c.o.style(Green()) << (pulling ? "[GIT PULL]" : "[GIT FETCH]") << c.o.style(Reset()) << " " << url
        << " -> " << git.work_dir() << "\n";
    show_transport(rep);
    c.o << "  " << W{14} << "pack" << ": " << fmt_bytes(rep.pack_bytes) << ", " << rep.objects << " object(s), "
        << rep.written << " written here\n";
    for (auto& r : rep.refs) {
      if (r.name.rfind("refs/heads/", 0) != 0) continue;
      c.o << "  " << c.o.style(Cyan()) << r.oid << c.o.style(Reset()) << "  " << r.name
          << " -> refs/remotes/origin/" << r.name.substr(11) << "\n";
    }
    if (rep.diverged) {
      c.o << "  " << c.o.style(Red()) << "diverged" << c.o.style(Reset()) << ": " << rep.default_branch << " is at "
          << rep.local_before << " here and " << rep.head_oid
          << " there, and neither contains the other — this machine cannot merge two histories yet\n";
    } else if (pulling && !rep.head_oid.empty() && rep.local_before != rep.head_oid) {
      c.o << "  " << W{14} << "branch moved" << ": " << (rep.local_before.empty() ? std::string("(unborn)") : rep.local_before)
          << " -> " << rep.head_oid << ", " << rep.files << " file(s) laid down\n";
    } else if (pulling) {
      c.o << "  " << W{14} << "branch" << ": already at " << rep.head_oid << "\n";
    }
    c.produced(rep.head_oid);
    remember(true);
    return;
  }

  if (sub == "add" || sub == "stage") {
    if (a.size() < 3) fail(Status::Invalid, "git add <path>");
    auto added = need(git.add(c.r.expand(a[2])));
    c.o << c.o.style(Header()) << "Staged " << added.size() << " path(s) into " << git.git_dir() << "/index (DIRC v2)" << c.o.style(Reset()) << "\n";
    for (auto& e : added) c.o << "  " << c.o.style(Cyan()) << e.oid << c.o.style(Reset()) << "  " << e.path << "\n";
    if (!added.empty()) c.produced(added.back().oid);
  } else if (sub == "commit" || sub == "ci") {
    std::string msg;
    for (size_t i = 2; i < a.size(); ++i) {
      if (a[i].rfind("as=", 0) == 0) continue;   // who is committing, not what the message says
      msg += (msg.empty() ? "" : " ") + c.r.expand(a[i]);
    }
    if (msg.empty()) fail(Status::Invalid, "git commit <message...>");
    auto cm = need(git.commit(msg));
    // The kernel's record of this repository moves with it: HEAD, branch, count.
    {
      userland::GitRepoRecord rec;
      rec.path = git.work_dir();
      size_t slash = rec.path.rfind('/');
      rec.name = slash == std::string::npos ? rec.path : rec.path.substr(slash + 1);
      rec.branch = git.branch();
      rec.head = cm.oid;
      rec.registered_ns = c.k.now();
      const userland::GitRepoRecord* prev = reg.find(rec.path);
      rec.commits = (prev ? prev->commits : 0) + 1;
      need(reg.record(rec, true));
    }
    c.produced(cm.oid);
    c.o << c.o.style(Green()) << "[COMMIT]" << c.o.style(Reset()) << " " << c.o.style(Cyan()) << cm.oid << c.o.style(Reset())
        << " on " << (git.branch().empty() ? "HEAD" : git.branch()) << (cm.parents.empty() ? " (root commit)" : "") << "\n";
    c.o << "  " << W{10} << "tree" << ": " << cm.tree << "\n";
    if (!cm.parents.empty()) c.o << "  " << W{10} << "parent" << ": " << cm.parents[0] << "\n";
    c.o << "  " << W{10} << "author" << ": " << cm.author << "  " << git_when(cm.author_time, cm.author_tz) << "\n";
    c.o << "  " << W{10} << "message" << ": " << cm.message << "\n";
  } else if (sub == "log" || sub == "history") {
    auto commits = need(git.log(has_kv(a, "max") ? (size_t)to_u64(kv(a, "max")) : 50));
    if (commits.empty()) { c.o << "no commits yet on " << (git.branch().empty() ? "HEAD" : git.branch()) << "\n"; return; }
    for (size_t i = 0; i < commits.size(); ++i) {
      auto& cm = commits[i];
      c.o << c.o.style(Yellow()) << "commit " << cm.oid << c.o.style(Reset());
      if (i == 0 && !git.branch().empty()) c.o << " (HEAD -> " << git.branch() << ")";
      c.o << "\n";
      c.o << "Author: " << cm.author << "\n";
      c.o << "Date:   " << git_when(cm.author_time, cm.author_tz) << "\n";
      c.o << "Tree:   " << cm.tree << "\n\n";
      for (auto& line : split(cm.message, '\n')) c.o << "    " << line << "\n";
      c.o << "\n";
    }
  } else if (sub == "status" || sub == "st") {
    auto st = need(git.status());
    c.o << c.o.style(Header()) << "On branch " << (st.branch.empty() ? "(detached HEAD)" : st.branch) << " — " << git.work_dir() << c.o.style(Reset()) << "\n";
    if (!st.has_commits) c.o << "  no commits yet\n";
    auto section = [&](const char* title, const std::vector<userland::GitStatusEntry>& v) {
      if (v.empty()) return;
      c.o << "  " << c.o.style(Bold()) << title << c.o.style(Reset()) << "\n";
      for (auto& e : v) c.o << "    " << W{12} << userland::git_change_name(e.change) << "  " << e.path << "\n";
    };
    section("staged (HEAD -> index)", st.staged);
    section("not staged (index -> work tree)", st.worktree);
    section("untracked", st.untracked);
    if (st.staged.empty() && st.worktree.empty() && st.untracked.empty()) c.o << "  nothing to commit, work tree clean\n";
  } else if (sub == "ls-files" || sub == "index") {
    // The staging area as the kernel read it out of .git/index — the same file
    // real git writes, so this lists what `git ls-files -s` lists.
    c.o << c.o.style(Header()) << "Index of " << git.work_dir() << " (" << git.index().size() << " entries, DIRC v2)" << c.o.style(Reset()) << "\n";
    for (auto& e : git.index()) c.o << "  " << W{8} << "100644" << " " << c.o.style(Cyan()) << e.oid << c.o.style(Reset()) << "  " << e.path << "\n";
  } else if (sub == "cat-file" || sub == "cat" || sub == "show") {
    if (a.size() < 3) fail(Status::Invalid, "git cat-file <oid>");
    // Takes what git takes here: HEAD, a branch or tag name, a full id, a prefix.
    std::string oid = need(git.resolve(c.r.expand(a[2])));
    auto o = need(git.read_object(oid));
    c.produced(o.oid);
    c.o << c.o.style(Header()) << o.type << " " << o.oid << " (" << o.payload.size() << " bytes)" << c.o.style(Reset()) << "\n";
    if (o.type == "tree") {
      auto ents = need(git.read_tree(o.oid));
      for (auto& e : ents) c.o << "  " << W{8} << (e.is_dir() ? "40000" : "100644") << " " << (e.is_dir() ? "tree" : "blob") << " " << e.oid << "  " << e.name << "\n";
    } else {
      c.o << string_of(ByteSpan(o.payload.data(), o.payload.size()));
      if (!o.payload.empty() && o.payload.back() != '\n') c.o << "\n";
    }
  } else if (sub == "fsck" || sub == "verify") {
    // Every loose object inflated, its header checked and its SHA-1 re-derived.
    auto rep = need(git.fsck());
    c.o << c.o.style(Header()) << "fsck " << git.git_dir() << ": " << rep.objects << " object(s), loose and packed — "
        << rep.commits << " commit, " << rep.trees << " tree, " << rep.blobs << " blob, " << rep.other << " other"
        << c.o.style(Reset()) << "\n";
    for (auto& b : rep.broken) c.o << "  " << c.o.style(Red()) << "corrupt object" << c.o.style(Reset()) << " " << b << "\n";
    if (!rep.broken.empty())
      fail(Status::Corrupt, "fsck: " + std::to_string(rep.broken.size()) + " corrupt object(s) in " + git.git_dir() + " — the database is damaged, not the reader");
    c.o << "  every object inflated, hashed to its own name\n";
  } else {
    // Anything this command does not do itself is the full git CLI's, run in the repository this
    // console is on (or where the process is), exactly as `aios git ...` or `aios-git ...` would.
    std::vector<std::string> gargs;
    gargs.push_back("git");
    for (size_t i = 1; i < a.size(); ++i) gargs.push_back(a[i]);
    std::string cwd = git.work_dir();
    if (cwd.empty()) { char buf[4096]; cwd = ::getcwd(buf, sizeof buf) ? std::string(buf) : std::string("."); }
    std::ostringstream gout, gerr;
    std::istringstream gin;
    int rc = aios_git::run(gargs, cwd, gout, gerr, gin);
    c.o << gout.str();
    if (rc != 0) fail(Status::Invalid, "git " + sub + ": " + (gerr.str().empty() ? "exit " + std::to_string(rc) : gerr.str()));
    if (!gerr.str().empty()) c.o << gerr.str();
  }
}

void cmd_sql(Ctx& c, const Args& a) {
  c.view();
  // The machine's own engine (machine_sql.hpp): the one fleet peers' sql_on
  // reaches, compute serve's when it runs in this process. `sql query` used to
  // open a second engine on the same store for every command - two engines
  // on one store, its MIRROR/MACHINES Things gone with it before their first
  // pull, and its ON <machine> / ON ALL without the fleet hook ("not in a
  // fleet") until something else had opened the machine engine.
  userland::SqlDatabase* machine = userland::machine_sql(c.k);
  std::unique_ptr<userland::TaskLedger> own_ledger;
  std::unique_ptr<userland::SqlDatabase> own_db;
  if (!machine) {   // the store did not open as the machine engine: a private one, as before
    own_ledger = std::make_unique<userland::TaskLedger>(c.k);
    own_ledger->init("tasks");
    own_db = std::make_unique<userland::SqlDatabase>(c.k, own_ledger.get());
    own_db->init("sql_db");
    userland::install_things_fleet(c.k);
  }
  userland::SqlDatabase& db = machine ? *machine : *own_db;

  std::string sub = (a.size() > 1) ? a[1] : "tables";
  if (sub == "query" || sub == "exec") {
    std::string sql;
    if (a.size() > 2) {
      std::string raw_sql;
      for (size_t i = 2; i < a.size(); ++i) raw_sql += (i > 2 ? " " : "") + a[i];
      if (raw_sql.rfind("sql=", 0) == 0) sql = raw_sql.substr(4);
      else sql = raw_sql;
    }
    if (sql.empty()) fail(Status::Invalid, "sql query \"<sql_statement>\"");

    auto res = db.execute_query(sql);
    if (!res.success) {
      c.o << c.o.style(Red()) << "[SQL ERROR]" << c.o.style(Reset()) << " " << res.error_message << "\n";
    } else {
      c.o << c.o.style(Green()) << "[SQL RESULT]" << c.o.style(Reset()) << "\n" << res.formatted_table << "\n";
    }
  } else if (sub == "pull" || sub == "import") {
    std::string cid = kv(a, "customer", "");
    std::string sql = kv(a, "sql", "");
    std::string file_path = kv(a, "file", "");

    if (file_path.empty() && a.size() > 2) {
      for (size_t i = 2; i < a.size(); ++i) {
        if (a[i].rfind("file=", 0) == 0) {
          file_path = a[i].substr(5);
        } else if (a[i].rfind("sql=", 0) == 0) {
          sql = a[i].substr(4);
        } else if (a[i].rfind("customer=", 0) != 0) {
          if (file_path.empty() && (a[i].length() > 4 && a[i].substr(a[i].length() - 4) == ".sql")) {
            file_path = a[i];
          } else if (file_path.empty() && c.k.host().exists(a[i])) {
            file_path = a[i];
          } else {
            sql += (sql.empty() ? "" : " ") + a[i];
          }
        }
      }
    }

    userland::SqlQueryResult res;
    if (!file_path.empty()) {
      c.o << c.o.style(Header()) << "Importing MySQL/MariaDB SQL file: " << file_path << c.o.style(Reset()) << "\n";
      res = db.import_sql_file(file_path);
    } else if (!sql.empty()) {
      if (!cid.empty()) {
        res = db.pull_mariadb_sql(cid, sql);
      } else {
        res = db.import_mysql_sql(sql);
      }
    } else {
      fail(Status::Invalid, "asql import <file.sql> | asql import file=<path> | asql import sql=\"<statements>\"");
    }

    if (!res.success) {
      c.o << c.o.style(Red()) << "[IMPORT ERROR]" << c.o.style(Reset()) << " " << res.error_message << "\n";
    } else {
      c.o << c.o.style(Green()) << "[IMPORT SUCCESS]" << c.o.style(Reset()) << "\n" << res.formatted_table;
    }
  } else if (sub == "push" || sub == "dump" || sub == "export") {
    std::string cid = kv(a, "customer", "");
    std::string tbl = kv(a, "table", "tasks");
    std::string sql_out = need(db.push_mariadb_sql(cid, tbl));

    c.o << c.o.style(Header()) << "MariaDB SQL Sync Push (Customer: " << (cid.empty() ? "ALL" : cid) << ", Table: " << tbl << ")" << c.o.style(Reset()) << "\n";
    c.o << sql_out << "\n";
  } else if (sub == "sync") {
    std::string cid = kv(a, "customer", "");
    need(db.sync_with_task_ledger(cid));

    c.o << c.o.style(Green()) << "[SUCCESS]" << c.o.style(Reset()) << " Bi-directional task ledger <-> MariaDB SQL sync completed for customer: " << (cid.empty() ? "ALL" : cid) << "\n";
  } else if (sub == "tables" || sub == "list") {
    auto tables = db.list_tables();
    c.o << c.o.style(Header()) << "Standard SQL Engine Database Tables (" << tables.size() << " tables)" << c.o.style(Reset()) << "\n";
    for (auto& tbl : tables) {
      c.o << "  - " << c.o.style(Cyan()) << tbl << c.o.style(Reset()) << "\n";
    }
  } else if (sub == "serve") {
    uint16_t port = has_kv(a, "port") ? (uint16_t)to_u64(kv(a, "port")) : 3307;
    std::string bind_addr = has_kv(a, "bind") ? kv(a, "bind") : (has_kv(a, "public") && kv(a, "public") == "1" ? "0.0.0.0" : "0.0.0.0");
    userland::MysqlWireServer server(c.k, db, port, bind_addr);
    if (has_kv(a, "max_connections")) server.set_max_connections(to_u64(kv(a, "max_connections")));
    if (has_kv(a, "query_memory_mb")) db.set_query_memory_budget((size_t)to_u64(kv(a, "query_memory_mb")) << 20);
    auto s_res = server.start();
    if (!s_res.ok()) {
      c.o << c.o.style(Red()) << "[ERROR]" << c.o.style(Reset()) << " Failed to start MySQL Wire Server on " << bind_addr << ":" << port << ": " << s_res.error().message << "\n";
      return;
    }
    c.o << c.o.style(Green()) << "[SUCCESS]" << c.o.style(Reset()) << " AIOS ASQL / MySQL Native Wire Server listening on " << bind_addr << ":" << port << " (Press Ctrl+C to stop)\n";
    c.o << "Ready for Navicat, DBeaver, TablePlus, and MySQL CLI connections.\n";
    while (true) {
      server.poll_and_serve(100);
    }
    return;
  } else if (sub == "console" || sub == "repl" || sub == "cli") {
    auto trim_str = [](const std::string& s) -> std::string {
      size_t start = 0;
      while (start < s.size() && (std::isspace(static_cast<unsigned char>(s[start])) != 0)) ++start;
      size_t end = s.size();
      while (end > start && (std::isspace(static_cast<unsigned char>(s[end - 1])) != 0)) --end;
      return s.substr(start, end - start);
    };
    auto upper_str = [](std::string s) -> std::string {
      for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      return s;
    };

    std::string active_db = has_kv(a, "db") ? kv(a, "db") : db.effective_database();
    if (active_db.empty()) active_db = "aios_db";
    c.o << c.o.style(Header()) << "Welcome to AIOS ASQL / MySQL Native Wire Console" << c.o.style(Reset()) << "\n";
    c.o << "Server version: 5.7.44-aios-asql (AIOS in-memory relational engine)\n";
    c.o << "Type 'help' or '\\h' for help. Type 'exit', 'quit', or '\\q' to exit.\n\n";
    std::string buffer;
    while (true) {
      if (buffer.empty()) {
        std::cout << "asql [" << active_db << "]> " << std::flush;
      } else {
        std::cout << "    -> " << std::flush;
      }
      std::string line;
      if (!std::getline(std::cin, line)) break;
      std::string trimmed = trim_str(line);
      if (buffer.empty()) {
        if (trimmed == "exit" || trimmed == "quit" || trimmed == "\\q" || trimmed == "\\q;") {
          std::cout << "Bye\n";
          break;
        }
        if (trimmed == "help" || trimmed == "\\h" || trimmed == "\\h;") {
          std::cout << "Commands:\n";
          std::cout << "  USE <database>;        Switch active tenant database\n";
          std::cout << "  SHOW DATABASES;        List available databases\n";
          std::cout << "  SHOW TABLES;           List tables in current database\n";
          std::cout << "  DESCRIBE <table>;      Describe columns of a table\n";
          std::cout << "  SELECT ...;            Run SQL / ASQL query\n";
          std::cout << "  \\q or exit            Exit console\n\n";
          continue;
        }
        if (trimmed == "clear" || trimmed == "\\c") {
          buffer.clear();
          continue;
        }
      }
      if (trimmed.empty()) continue;
      buffer += (buffer.empty() ? "" : " ") + trimmed;
      if (buffer.back() == ';' || buffer.back() == '\\') {
        std::string sql = buffer;
        if (sql.back() == ';') sql.pop_back();
        buffer.clear();
        sql = trim_str(sql);
        if (sql.empty()) continue;

        auto start = std::chrono::steady_clock::now();
        auto res = db.execute_gateway_query(sql, active_db);
        auto end = std::chrono::steady_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();

        std::string up_sql = upper_str(sql);
        if (up_sql.rfind("USE ", 0) == 0) {
          std::string new_db = trim_str(sql.substr(4));
          while (!new_db.empty() && (new_db.back() == ';' || new_db.back() == '`' || new_db.back() == '\'' || new_db.back() == '"')) new_db.pop_back();
          while (!new_db.empty() && (new_db.front() == '`' || new_db.front() == '\'' || new_db.front() == '"')) new_db.erase(new_db.begin());
          if (!new_db.empty()) active_db = new_db;
        }

        if (!res.success) {
          std::cout << "ERROR 1064 (42000): " << res.error_message << "\n";
        } else if (res.is_result_set) {
          std::cout << res.formatted_table << "\n";
          std::cout << res.rows.size() << (res.rows.size() == 1 ? " row" : " rows") << " in set (" << std::fixed << std::setprecision(2) << elapsed_ms << " ms)\n\n";
        } else {
          std::cout << "Query OK, " << res.rows_affected << (res.rows_affected == 1 ? " row affected" : " rows affected") << " (" << std::fixed << std::setprecision(2) << elapsed_ms << " ms)\n\n";
        }
      }
    }
    return;
  } else {
    fail(Status::Invalid, "sql query \"<sql>\" | sql console | sql serve [port=3307] | sql pull customer=<cid> sql=\"<sql>\" | sql push customer=<cid> [table=tasks] | sql sync [customer=<cid>] | sql tables");
  }
}

void cmd_ingress(Ctx& c, const Args& a) {
  c.view();
  Surface actor = has_kv(a, "as") ? c.as(kv(a, "as")) : c.me;
  channel::IngressReceiver receiver(c.k, &actor);
  // `receive file=x` and `upload file=x` are `ingress receive file=x`: the alias
  // names the subcommand, so it is not repeated.
  static const char* const subs[] = {"receive", "file", "upload", "ingest", "data", "payload",
                                     "log",     "list", "status", "verify"};
  std::string sub;
  size_t first = 1;
  if (a.size() > 1 && std::find(std::begin(subs), std::end(subs), a[1]) != std::end(subs)) {
    sub = a[1];
    first = 2;
  } else if (a[0] == "receive" || a[0] == "upload") {
    sub = "receive";
  } else if (a.size() > 1 && a[1].find('=') == std::string::npos) {
    fail(Status::Invalid, "ingress: unknown subcommand '" + a[1] + "'");
  } else {
    sub = "log";
  }
  std::vector<std::string> pos;
  for (size_t i = first; i < a.size(); ++i) if (a[i].find('=') == std::string::npos) pos.push_back(a[i]);

  auto show = [&](const channel::IngressPayload& p, const char* what) {
    bool good = p.status == "VERIFIED" || p.status == "INGESTED";
    c.o << c.o.style(good ? Green() : Red()) << what << c.o.style(Reset()) << "\n";
    c.o << "  " << W{20} << "Payload ID" << ": " << p.payload_id << "\n";
    c.o << "  " << W{20} << "Customer ID" << ": " << p.customer_id << "\n";
    c.o << "  " << W{20} << "Source" << ": " << p.source << "\n";
    c.o << "  " << W{20} << "Filename" << ": " << p.filename << "\n";
    c.o << "  " << W{20} << "Content Type" << ": " << p.content_type << "\n";
    c.o << "  " << W{20} << "Size (Bytes)" << ": " << p.size_bytes << "\n";
    c.o << "  " << W{20} << "SHA-256" << ": " << p.sha256_checksum << "\n";
    c.o << "  " << W{20} << "Status" << ": " << p.status << (p.detail.empty() ? "" : " — " + p.detail) << "\n";
    c.o << "  " << W{20} << "Staged Path" << ": " << p.target_path << "\n";
    c.o << "  " << W{20} << "Received" << ": " << civil_date(p.timestamp_ns) << " (wall clock, "
        << (p.timestamp_ns / 1000000000ull) << " s since 1970)\n";
  };

  if (sub == "receive" || sub == "file" || sub == "upload") {
    std::string path = kv(a, "file", pos.empty() ? std::string() : pos[0]);
    std::string cid = kv(a, "customer", "default");
    std::string target = kv(a, "target", "");
    if (path.empty()) fail(Status::Invalid, "ingress receive file=<path> [customer=<cid>] [target=<name>]");
    auto p = need(receiver.receive_file(path, cid, target));
    show(p, p.content_type == "text/x-sql" ? "[STAGED FROM LOCAL FILE; SQL LOADED]" : "[STAGED FROM LOCAL FILE]");
    if (p.status == "FAILED") fail(Status::Invalid, "the database refused " + p.payload_id + ": " + p.detail);
  } else if (sub == "ingest" || sub == "data" || sub == "payload") {
    std::string cid = kv(a, "customer", "default");
    std::string fname = kv(a, "filename", "data.sql");
    std::string data = kv(a, "payload", "");
    if (data.empty()) for (const auto& w : pos) data += (data.empty() ? "" : " ") + w;
    if (data.empty()) fail(Status::Invalid, "ingress ingest customer=<cid> [filename=<name>] payload=\"<data>\"");
    auto p = need(receiver.receive_string(fname, cid, data, ""));
    show(p, p.content_type == "text/x-sql" ? "[STAGED INLINE; SQL LOADED]" : "[STAGED INLINE]");
    if (p.status == "FAILED") fail(Status::Invalid, "the database refused " + p.payload_id + ": " + p.detail);
  } else if (sub == "log" || sub == "list" || sub == "status") {
    c.o << receiver.render_ingress_log(kv(a, "customer", ""));
  } else if (sub == "verify") {
    std::string pid = kv(a, "id", pos.empty() ? std::string() : pos[0]);
    if (pid.empty()) fail(Status::Invalid, "ingress verify <payload_id>");
    need(receiver.verify_payload(pid));
    c.o << c.o.style(Green()) << "[VERIFIED]" << c.o.style(Reset()) << " the staged file for " << pid
        << " still hashes to its recorded SHA-256\n";
  } else {
    fail(Status::Invalid, "ingress receive file=<path> [customer=<cid>] [target=<name>] | ingress ingest customer=<cid> payload=\"<data>\" | ingress log [customer=<cid>] | ingress verify <payload_id>");
  }
}

// Where the installer script is on THIS machine. The three call sites below
// each named one source tree's scripts/install-dualboot.sh outright, so
// `install` could only ever work inside one person's source tree -- on any
// machine that installed AIOS it ran a path that does not exist and reported
// the shell's 127 as an install failure.
#if !defined(AIOS_METAL)
std::string install_script() {
  const char* named = std::getenv("AOS_INSTALL_SCRIPT");
  if (named && *named) return named;
  struct stat st;
  for (const char* p : {"scripts/install-dualboot.sh",
                        "/usr/local/lib/aios/install-dualboot.sh",
                        "/repos/aios/scripts/install-dualboot.sh"})
    if (stat(p, &st) == 0) return p;
  return "scripts/install-dualboot.sh";
}
#endif

void cmd_mail(Ctx& c, const Args& a) {
  c.view();
  userland::MailStore store(c.k.config().root + "/mail", &c.k.host(), c.k.config().root);

  std::string sub = (a.size() > 1) ? a[1] : "list";
  if (sub == "search" || sub == "find" || sub == "query") {
    if (a.size() < 3) fail(Status::Invalid, "mail search [--server] <query> (e.g. 'from:linode', 'subject:lag', 'body:invoice', 'has:attachment')");
    // --server hands the query to the mail server instead of the local index:
    // full text, and mail far older than anything we keep envelopes for. On
    // Gmail that is X-GM-RAW over All Mail, so the whole Gmail query language
    // works. The local index covers sender/subject/date across everything
    // synced, instantly and offline.
    bool server_side = false;
    std::string query;
    for (size_t i = 2; i < a.size(); ++i) {
      if (a[i] == "--server" || a[i] == "-s") { server_side = true; continue; }
      query += (query.empty() ? "" : " ") + a[i];
    }
    if (query.empty()) fail(Status::Invalid, "mail search [--server] <query>");
    if (server_side) {
      auto res = store.search_server(query);
      if (!res.ok()) fail(Status::IoError, res.error().str());
      c.o << c.o.style(Header()) << "Server search: \"" << query << "\" (" << res->size() << " matches)" << c.o.style(Reset()) << "\n";
      for (const auto& m : *res) {
        c.o << (m.is_unread ? "* " : "  ") << "[uid " << m.uid << "] " << m.date_str << "  "
            << m.from_name << " <" << m.from_addr << "> : " << m.subject << "\n";
      }
      return;
    }
    auto msgs = store.get_messages("all", query, 0, "all");
    size_t shown = msgs.size() > 40 ? 40 : msgs.size();
    c.o << c.o.style(Header()) << "AetherMail Search: \"" << query << "\" ("
        << (shown < msgs.size() ? "showing " + std::to_string(shown) + " of " : "")
        << msgs.size() << " matches)" << c.o.style(Reset()) << "\n";
    msgs.resize(shown);
    for (const auto& m : msgs) {
      c.o << (m.is_unread ? "* " : "  ")
          << "[" << m.id << "] "
          << (m.is_flagged ? "[★] " : "")
          << (!m.attachments.empty() ? "[📎] " : "")
          << "[" << m.folder << "] "
          << m.date_str << "  "
          << m.from_name << " <" << m.from_addr << "> : "
          << m.subject << "\n";
    }
  } else if (sub == "list" || sub == "ls") {
    std::string folder = (a.size() > 2 && a[2][0] != '-') ? a[2] : "inbox";
    auto get_opt = [&](const std::string& key, const std::string& def = "") -> std::string {
      for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].rfind(key + "=", 0) == 0) return a[i].substr(key.size() + 1);
        if (a[i].rfind("--" + key + "=", 0) == 0) return a[i].substr(key.size() + 3);
        if ((a[i] == "--" + key || a[i] == "-" + key) && i + 1 < a.size()) return a[i + 1];
      }
      return def;
    };
    auto has_opt = [&](const std::string& key) -> bool {
      for (const auto& t : a) {
        if (t == "--" + key || t == "-" + key || t.rfind(key + "=", 0) == 0 || t.rfind("--" + key + "=", 0) == 0) return true;
      }
      return false;
    };

    std::string search = get_opt("q", kv(a, "q", ""));
    std::string from_f = get_opt("from", kv(a, "from", ""));
    std::string to_f = get_opt("to", kv(a, "to", ""));
    std::string subj_f = get_opt("subject", kv(a, "subject", ""));
    if (subj_f.empty()) subj_f = get_opt("subj", kv(a, "subj", ""));
    std::string body_f = get_opt("body", kv(a, "body", ""));
    bool has_att = has_opt("has-attachment") || has_opt("has-attach") || has_opt("attach") || has_opt("files");
    bool unread = has_opt("unread");
    bool flagged = has_opt("flagged") || has_opt("star");
    std::string acc = get_opt("account", kv(a, "account", "all"));

    std::string full_q = search;
    if (!from_f.empty()) full_q += (full_q.empty() ? "" : " ") + ("from:\"" + from_f + "\"");
    if (!to_f.empty()) full_q += (full_q.empty() ? "" : " ") + ("to:\"" + to_f + "\"");
    if (!subj_f.empty()) full_q += (full_q.empty() ? "" : " ") + ("subject:\"" + subj_f + "\"");
    if (!body_f.empty()) full_q += (full_q.empty() ? "" : " ") + ("body:\"" + body_f + "\"");
    if (has_att) full_q += (full_q.empty() ? "" : " ") + std::string("has:attachment");
    if (unread) full_q += (full_q.empty() ? "" : " ") + std::string("is:unread");
    if (flagged) full_q += (full_q.empty() ? "" : " ") + std::string("is:flagged");

    auto msgs = store.get_messages(folder, full_q, 0, acc);
    // The index goes back to the start of the archive -- tens of thousands of
    // messages -- so a bare `mail list` shows the newest page, not all of it.
    size_t limit = 40;
    std::string limit_s = get_opt("limit", kv(a, "limit", ""));
    if (!limit_s.empty()) limit = (size_t)std::atoll(limit_s.c_str());
    if (limit == 0 || limit > msgs.size()) limit = msgs.size();
    c.o << c.o.style(Header()) << "AetherMail - Folder: " << folder
        << (!full_q.empty() ? (" [Filter: " + full_q + "]") : "")
        << " (showing " << limit << " of " << msgs.size() << " messages)" << c.o.style(Reset()) << "\n";
    msgs.resize(limit);
    for (const auto& m : msgs) {
      c.o << (m.is_unread ? "* " : "  ")
          << "[" << m.id << "] "
          << (m.is_flagged ? "[★] " : "")
          << (!m.attachments.empty() ? "[📎] " : "")
          << m.date_str << "  "
          << m.from_name << " <" << m.from_addr << "> : "
          << m.subject << "\n";
    }
  } else if (sub == "read" || sub == "cat" || sub == "view" || sub == "show") {
    if (a.size() < 3) fail(Status::Invalid, "mail read <message-id>");
    std::string id = a[2];
    const userland::MailMessage* m = store.get_message(id);
    if (!m) fail(Status::NotFound, "no message with id: " + id);
    store.mark_read(id, true);
    c.o << c.o.style(Header()) << "Subject: " << m->subject << c.o.style(Reset()) << "\n";
    c.o << "From:    " << m->from_name << " <" << m->from_addr << ">\n";
    c.o << "Date:    " << m->date_str << "\n";
    std::string to_s;
    for (size_t i = 0; i < m->to.size(); ++i) to_s += (i > 0 ? ", " : "") + m->to[i];
    c.o << "To:      " << to_s << "\n";
    if (!m->attachments.empty()) {
      c.o << "Att:     ";
      for (size_t i = 0; i < m->attachments.size(); ++i) {
        c.o << (i > 0 ? ", " : "") << m->attachments[i].filename << " (" << (m->attachments[i].size_bytes / 1024 + 1) << " KB)";
      }
      c.o << "\n";
    }
    c.o << "------------------------------------------------------------\n";
    if (m->body.empty()) {
      // The vacuum evicts bodies past the retention window; the envelope stays
      // and the message is fetched back by its uid the moment it is opened.
      auto got = store.fetch_body(id);
      if (!got.ok()) {
        c.o << "(body not cached locally and could not be fetched: " << got.error().str() << ")\n";
      }
      m = store.get_message(id);
    }
    if (m) c.o << m->body << "\n";
  } else if (sub == "send") {
    if (a.size() < 5) fail(Status::Invalid, "mail send <to> <subject> <body>");
    std::string to = a[2];
    std::string subject = a[3];
    std::string body;
    for (size_t i = 4; i < a.size(); ++i) body += (i > 4 ? " " : "") + a[i];
    std::string id = store.send_message(store.primary_account().email, {to}, {}, subject, body);
    // Say what actually happened: delivered by SMTP, or only kept in Sent.
    const std::string what = store.status_line();
    const bool delivered = what.find("[SMTP Delivered]") != std::string::npos;
    c.o << c.o.style(delivered ? Green() : Red()) << (delivered ? "[SENT]" : "[NOT SENT]") << c.o.style(Reset())
        << " Message " << id << ": " << what << "\n";
  } else if (sub == "flag" || sub == "star") {
    if (a.size() < 3) fail(Status::Invalid, "mail flag <message-id>");
    if (store.toggle_flag(a[2])) {
      c.o << "Toggled flag for " << a[2] << "\n";
    } else {
      fail(Status::NotFound, "no message with id: " + a[2]);
    }
  } else if (sub == "delete" || sub == "rm" || sub == "trash") {
    if (a.size() < 3) fail(Status::Invalid, "mail delete <message-id>");
    if (store.delete_message(a[2])) {
      c.o << "Deleted message " << a[2] << "\n";
    } else {
      fail(Status::NotFound, "no message with id: " + a[2]);
    }
  } else if (sub == "archive") {
    if (a.size() < 3) fail(Status::Invalid, "mail archive <message-id>");
    if (store.archive_message(a[2])) {
      c.o << "Archived message " << a[2] << "\n";
    } else {
      fail(Status::NotFound, "no message with id: " + a[2]);
    }
  } else if (sub == "folders" || sub == "boxes") {
    auto folders = store.get_folders();
    c.o << c.o.style(Header()) << "AetherMail Mailboxes" << c.o.style(Reset()) << "\n";
    for (const auto& f : folders) {
      c.o << "  " << f.name << " (" << f.id << "): " << f.total_count << " total, " << f.unread_count << " unread\n";
    }
  } else if (sub == "status") {
    c.o << c.o.style(Header()) << "AetherMail System Status" << c.o.style(Reset()) << "\n";
    if (store.accounts().empty()) {
      c.o << "Primary Account: none (add one: mail account add --email <addr> ...)\n";
    } else {
      c.o << "Primary Account: " << store.primary_account().display_name << " <" << store.primary_account().email << ">\n";
    }
    c.o << "Accounts:        " << store.accounts().size() << " configured\n";
    c.o << "Status:          " << store.status_line() << "\n";
    c.o << "Total Unread:    " << store.total_unread() << " messages\n";
    c.o << "Envelopes:       " << store.get_messages("all", "", 0, "all").size() << " indexed (kept indefinitely)\n";
    c.o << "Body cache:      last " << store.body_retain_days() << " days, capped at "
        << (store.body_cache_cap_bytes() / (1024 * 1024)) << " MB\n";
    for (const auto& st : store.sync_state()) {
      c.o << "  " << st.account_id << "/" << st.folder
          << ": uid " << st.last_uid
          << (st.backfilled ? " (backfilled)" : " (backfill pending)") << "\n";
    }
  } else if (sub == "vacuum" || sub == "gc") {
    // Both knobs are settable so the policy can be tuned, and so that what the
    // vacuum does can actually be driven rather than assumed.
    int days = store.body_retain_days();
    uint64_t cap_mb = store.body_cache_cap_bytes() / (1024 * 1024);
    for (size_t i = 2; i < a.size(); ++i) {
      if ((a[i] == "--days" || a[i] == "-d") && i + 1 < a.size()) days = std::atoi(a[++i].c_str());
      else if ((a[i] == "--cap" || a[i] == "-c") && i + 1 < a.size()) cap_mb = (uint64_t)std::atoll(a[++i].c_str());
    }
    store.set_body_retention(days, cap_mb * 1024 * 1024);
    size_t before = 0;
    for (const auto& m : store.get_messages("all", "", 0, "all")) (void)m, before++;
    size_t removed = store.vacuum_bodies();
    store.save();
    c.o << "Policy: keep bodies " << days << " days, cap " << cap_mb << " MB\n";
    c.o << c.o.style(Green()) << "[VACUUM]" << c.o.style(Reset()) << " Evicted " << removed
        << " cached message bod" << (removed == 1 ? "y" : "ies") << "; "
        << before << " envelopes kept.\n";
  } else if (sub == "sync" || sub == "check") {
    store.check_new_mail();
    c.o << c.o.style(Green()) << "[SYNC]" << c.o.style(Reset()) << " Mailbox check completed: " << store.status_line() << "\n";
  } else if (sub == "account" || sub == "accounts" || sub == "acc") {
    std::string act = (a.size() > 2) ? a[2] : "list";
    if (act == "list" || act == "ls") {
      c.o << c.o.style(Header()) << "Configured Mail Accounts (" << store.accounts().size() << ")" << c.o.style(Reset()) << "\n";
      for (const auto& acc : store.accounts()) {
        c.o << (acc.is_default ? "* " : "  ")
            << "[" << acc.id << "] "
            << acc.display_name << " <" << acc.email << ">\n"
            << "     Provider: " << acc.provider
            << " | IMAP: " << acc.incoming_server << ":" << acc.incoming_port
            << " | SMTP: " << acc.outgoing_server << ":" << acc.outgoing_port
            << " | User: " << acc.username << "\n";
      }
    } else if (act == "add") {
      auto get_flag = [&](const std::string& key) -> std::string {
        std::string v = kv(a, key, "");
        if (!v.empty()) return v;
        std::string flag = "--" + key;
        for (size_t i = 0; i < a.size(); ++i) {
          if (a[i] == flag && i + 1 < a.size()) return a[i + 1];
          if (a[i].rfind(flag + "=", 0) == 0) return a[i].substr(flag.size() + 1);
        }
        return "";
      };

      std::string email = get_flag("email");
      if (email.empty() && a.size() > 3 && a[3].find('@') != std::string::npos && a[3].find("--") == std::string::npos) email = a[3];
      if (email.empty()) fail(Status::Invalid, "mail account add --email <addr> [--preset <icloud|gmail|outlook|fastmail|yahoo|custom>] [--name <name>] [--user <user>] [--pass <pass>] [--imap host:port] [--smtp host:port]");

      std::string preset = get_flag("preset");
      std::string pass = get_flag("pass");
      std::string name = get_flag("name");
      std::string user = get_flag("user");
      if (user.empty()) user = email;

      userland::MailAccount acc;
      if (!preset.empty()) {
        acc = userland::MailAccount::make_preset(preset, email, pass, name);
      } else {
        // Auto-detect preset by domain if not given
        if (email.find("@icloud.com") != std::string::npos || email.find("@me.com") != std::string::npos) {
          acc = userland::MailAccount::make_preset("icloud", email, pass, name);
        } else if (email.find("@gmail.com") != std::string::npos || email.find("@googlemail.com") != std::string::npos) {
          acc = userland::MailAccount::make_preset("gmail", email, pass, name);
        } else if (email.find("@outlook.com") != std::string::npos || email.find("@hotmail.com") != std::string::npos || email.find("@live.com") != std::string::npos) {
          acc = userland::MailAccount::make_preset("outlook", email, pass, name);
        } else if (email.find("@fastmail.com") != std::string::npos) {
          acc = userland::MailAccount::make_preset("fastmail", email, pass, name);
        } else if (email.find("@yahoo.com") != std::string::npos) {
          acc = userland::MailAccount::make_preset("yahoo", email, pass, name);
        } else {
          acc = userland::MailAccount::make_preset("custom", email, pass, name);
        }
      }

      std::string imap_arg = get_flag("imap");
      if (!imap_arg.empty()) {
        size_t cp = imap_arg.find(':');
        if (cp != std::string::npos) {
          acc.incoming_server = imap_arg.substr(0, cp);
          acc.incoming_port = (uint16_t)std::atoi(imap_arg.substr(cp + 1).c_str());
        } else {
          acc.incoming_server = imap_arg;
        }
      }

      std::string smtp_arg = get_flag("smtp");
      if (!smtp_arg.empty()) {
        size_t cp = smtp_arg.find(':');
        if (cp != std::string::npos) {
          acc.outgoing_server = smtp_arg.substr(0, cp);
          acc.outgoing_port = (uint16_t)std::atoi(smtp_arg.substr(cp + 1).c_str());
        } else {
          acc.outgoing_server = smtp_arg;
        }
      }

      if (!user.empty()) acc.username = user;
      if (!pass.empty()) acc.password = pass;
      if (!name.empty()) acc.display_name = name;

      store.add_account(acc);
      c.o << c.o.style(Green()) << "[SAVED]" << c.o.style(Reset()) << " Account added successfully: " << acc.display_name << " (" << acc.email << ")\n";
      c.o << "  Incoming IMAP: " << acc.incoming_server << ":" << acc.incoming_port << "\n";
      c.o << "  Outgoing SMTP: " << acc.outgoing_server << ":" << acc.outgoing_port << "\n";
    } else if (act == "preset") {
      if (a.size() < 5) fail(Status::Invalid, "mail account preset <icloud|gmail|outlook|fastmail|yahoo> <email> <password> [display_name]");
      std::string prov = a[3];
      std::string email = a[4];
      std::string pass = (a.size() > 5) ? a[5] : "";
      std::string disp = (a.size() > 6) ? a[6] : "";
      auto acc = userland::MailAccount::make_preset(prov, email, pass, disp);
      store.add_account(acc);
      c.o << c.o.style(Green()) << "[SAVED]" << c.o.style(Reset()) << " Added preset account: " << acc.display_name << " (" << acc.email << ")\n";
    } else if (act == "test") {
      if (a.size() < 4) fail(Status::Invalid, "mail account test <account-id-or-email>");
      std::string id = a[3];
      const auto* acc = store.get_account(id);
      if (!acc) fail(Status::NotFound, "no account with id or email: " + id);
      c.o << "Testing connectivity for " << acc->email << " (" << acc->incoming_server << " / " << acc->outgoing_server << ")...\n";
      auto res = store.test_account_connection(*acc);
      if (res.ok()) {
        c.o << c.o.style(Green()) << "[OK] " << c.o.style(Reset()) << *res << "\n";
      } else {
        // A failed test is a failed command (exit status and `!` in scripts).
        c.o << c.o.style(Red()) << "[FAIL] " << c.o.style(Reset()) << res.error().message << "\n";
        fail(Status::IoError, "mail account test: " + acc->email + " did not pass");
      }
    } else if (act == "remove" || act == "rm" || act == "delete") {
      if (a.size() < 4) fail(Status::Invalid, "mail account remove <account-id-or-email>");
      if (store.remove_account(a[3])) {
        c.o << "Removed account " << a[3] << "\n";
      } else {
        fail(Status::NotFound, "no account with id or email: " + a[3]);
      }
    } else if (act == "set-pass" || act == "password" || act == "passwd") {
      if (a.size() < 5) fail(Status::Invalid, "mail account set-pass <account-id-or-email> <app-password>");
      auto* acc = store.accounts_mut().empty() ? nullptr : store.get_account_mut(a[3]);
      if (!acc) fail(Status::NotFound, "no account with id or email: " + a[3]);
      acc->password = a[4];
      acc->last_sync_status = "Never synced";
      acc->last_sync_at = 0;
      store.save();
      c.o << c.o.style(Green()) << "[SAVED]" << c.o.style(Reset()) << " Password stored for " << acc->email
          << " (" << a[4].size() << " chars). Run 'mail account test " << acc->id << "' to verify.\n";
    } else if (act == "default" || act == "set-default") {
      if (a.size() < 4) fail(Status::Invalid, "mail account set-default <account-id-or-email>");
      if (store.set_default_account(a[3])) {
        c.o << "Set default account to " << a[3] << "\n";
      } else {
        fail(Status::NotFound, "no account with id or email: " + a[3]);
      }
    } else {
      fail(Status::Invalid, "mail account list | add --email <addr> [--preset ..] | preset <prov> <email> <pass> | set-pass <id> <pass> | test <id> | remove <id> | set-default <id>");
    }
  } else {
    fail(Status::Invalid, "mail list [folder] | search [--server] <query> | read <id> | send <to> <subj> <body> | account <list|add|preset|set-pass|test|remove> | sync | vacuum | status");
  }
}

// ---------------------------------------------------------------------------------------------------
// google: sign in to a Google account with OAuth 2.0, and keep its mail, calendar, contacts and notes in sync
// ---------------------------------------------------------------------------------------------------

namespace {

std::string google_pick_account(Ctx& c, const Args& a, userland::OAuth& oauth) {
  std::string want = kv(a, "account");
  if (want.empty())
    for (size_t i = 2; i < a.size(); ++i)
      if (a[i].find('@') != std::string::npos && a[i].find('=') == std::string::npos) want = a[i];
  auto toks = oauth.list_tokens();
  if (!toks.ok()) fail(toks.error().status, toks.error().message);
  std::vector<std::string> mine;
  for (const auto& t : *toks) if (t.provider == "google") mine.push_back(t.account);
  if (mine.empty()) fail(Status::NotFound, "no Google account is signed in: run `google auth you@gmail.com`");
  if (!want.empty()) {
    for (const auto& m : mine) if (m == want) return m;
    fail(Status::NotFound, want + " is not signed in (signed in: " + mine[0] + (mine.size() > 1 ? ", ..." : "") + ")");
  }
  if (mine.size() > 1) fail(Status::Invalid, "more than one Google account is signed in: say which with account=you@gmail.com");
  (void)c;
  return mine[0];
}

// The mail account that goes with a sign-in: Gmail's IMAP and SMTP, signing in with the OAuth token.
void google_ensure_mail_account(Ctx& c, const std::string& email) {
  userland::MailStore store(c.k.config().root + "/mail", &c.k.host(), c.k.config().root);
  userland::MailAccount acc = userland::MailAccount::make_preset("gmail", email, "", "");
  acc.auth = "oauth2";
  acc.username = email;
  acc.password.clear();
  // Gmail's servers, unless <root>/oauth/google.json names others (a Workspace relay, a test server).
  {
    userland::GoogleSync gs(c.k.host(), c.k.config().root, email);
    if (auto ep = gs.endpoints(); ep.ok()) {
      acc.incoming_server = ep->imap_host;
      acc.incoming_port = ep->imap_port;
      acc.outgoing_server = ep->smtp_host;
      acc.outgoing_port = ep->smtp_port;
      acc.use_tls = !(ep->imap_security == "none" && ep->smtp_security == "none");
    }
  }
  if (const userland::MailAccount* have = store.get_account(acc.id)) {
    userland::MailAccount upd = *have;
    upd.auth = "oauth2";
    upd.password.clear();
    upd.incoming_server = acc.incoming_server;
    upd.incoming_port = acc.incoming_port;
    upd.outgoing_server = acc.outgoing_server;
    upd.outgoing_port = acc.outgoing_port;
    upd.use_tls = acc.use_tls;
    upd.last_sync_status = "Never synced";
    store.update_account(upd);
  } else {
    store.add_account(acc);
  }
}

}  // namespace

void cmd_google(Ctx& c, const Args& a) {
  c.view();
  const std::string root = c.k.config().root;
  userland::OAuth oauth(c.k.host(), root);
  const std::string sub = (a.size() > 1) ? a[1] : "status";

  if (sub == "auth" || sub == "login" || sub == "signin") {
    auto cfg = oauth.load_config("google");
    if (!cfg.ok()) {
      c.o << c.o.style(Red()) << "[NO CLIENT]" << c.o.style(Reset()) << " " << cfg.error().message << "\n";
      c.o << "  1. In Google Cloud Console create (or pick) a project and enable the Google Calendar API and the People API.\n"
          << "  2. Credentials > Create credentials > OAuth client ID > Application type: Desktop app.\n"
          << "  3. Download its JSON and save it as " << root << "/oauth/google.json\n"
          << "  4. Run `google auth you@gmail.com` again.\n"
          << "  (On a Google Workspace domain, an Internal consent screen avoids the 7-day token expiry of apps in Testing.)\n";
      fail(Status::NotFound, "google auth: no OAuth client");
    }
    // `google auth finish <the address the browser ended up at>`: the sign-in was done in a browser elsewhere.
    if (a.size() > 2 && a[2] == "finish") {
      if (a.size() < 4) fail(Status::Invalid, "google auth finish <the full address from the browser's address bar>");
      std::string pasted;
      for (size_t i = 3; i < a.size(); ++i) pasted += (i > 3 ? " " : "") + a[i];
      auto pend = oauth.take_pending("google");
      if (!pend.ok()) fail(pend.error().status, pend.error().message);
      auto red = userland::OAuth::parse_redirect(pasted);
      if (!red.ok()) fail(red.error().status, red.error().message);
      auto tok = oauth.finish(*cfg, *pend, *red);
      if (!tok.ok()) fail(tok.error().status, tok.error().message);
      need(oauth.save_token(*tok));
      google_ensure_mail_account(c, tok->account);
      c.o << c.o.style(Green()) << "[SIGNED IN]" << c.o.style(Reset()) << " " << tok->account << "\n  next: `google sync`\n";
      return;
    }
    std::string hint;
    for (size_t i = 2; i < a.size(); ++i) if (a[i].find('@') != std::string::npos) hint = a[i];
    const bool manual = has_kv(a, "manual") || std::find(a.begin(), a.end(), std::string("--manual")) != a.end();

    auto lis = hal::net_listen(0, true);
    if (!lis.ok() && !manual) fail(lis.error().status, "cannot listen for the sign-in redirect: " + lis.error().message + " (use `google auth --manual`)");
    const std::string redirect = (manual || !lis.ok()) ? "http://127.0.0.1:1/" : "http://127.0.0.1:" + std::to_string((*lis)->port()) + "/";
    auto start = oauth.begin(*cfg, redirect, hint);
    if (!start.ok()) fail(start.error().status, start.error().message);
    need(oauth.save_pending(start->pending));
    c.o << "Open this address in a browser and sign in" << (hint.empty() ? "" : " as " + hint) << ":\n\n  " << start->url << "\n\n";
    if (manual) {
      c.o << "The browser will end up on a page that does not load (127.0.0.1). Copy that page's whole address from the address bar and run:\n"
          << "  google auth finish '<address>'\n";
      return;
    }
    c.o << "Waiting for the sign-in (up to 10 minutes). If the browser is on another machine, open the address there, then copy the address it\n"
        << "ends up on and run `google auth finish '<address>'` here.\n";
    std::cout.flush();
    auto red = oauth.wait_for_redirect(**lis, 600000);
    if (!red.ok()) fail(red.error().status, red.error().message);
    auto pend = oauth.take_pending("google");
    if (!pend.ok()) fail(pend.error().status, pend.error().message);
    auto tok = oauth.finish(*cfg, *pend, *red);
    if (!tok.ok()) fail(tok.error().status, tok.error().message);
    need(oauth.save_token(*tok));
    google_ensure_mail_account(c, tok->account);
    c.o << c.o.style(Green()) << "[SIGNED IN]" << c.o.style(Reset()) << " " << tok->account << "\n"
        << "  The tokens are in " << root << "/oauth/ (encrypted) and survive a reboot. A mail account for it is set up.\n  next: `google sync`\n";
    return;
  }

  if (sub == "status") {
    auto toks = oauth.list_tokens();
    if (!toks.ok()) fail(toks.error().status, toks.error().message);
    int n = 0;
    c.o << c.o.style(Header()) << "Google sign-ins" << c.o.style(Reset()) << "\n";
    const int64_t now = static_cast<int64_t>(c.k.host().wall_ns() / 1000000000ull);
    json::Value st;
    if (auto raw = c.k.host().read_file(root + "/google/state.json"); raw.ok()) {   // host-io: the machine's google sync state (userland::google_sync)
      auto doc = json::parse(std::string_view(reinterpret_cast<const char*>(raw->data()), raw->size()));
      if (doc.ok()) st = *doc;
    }
    for (const auto& t : *toks) {
      if (t.provider != "google") continue;
      ++n;
      c.o << "  " << t.account << "\n    access token: " << (t.expires_at > now ? "valid for " + std::to_string((t.expires_at - now) / 60) + " min" : std::string("expired (refreshed on use)"))
          << "   refresh token: " << (t.refresh_token.empty() ? "MISSING: sign in again" : "held") << "\n    scopes: " << t.scope << "\n";
      for (const char* part : {"calendar", "contacts", "notes"}) {
        int64_t last = st["accounts"][t.account][part]["last_sync"].i64();
        c.o << "    " << part << ": " << (last ? std::to_string((now - last) / 60) + " min ago" : std::string("never synced")) << "\n";
      }
      userland::MailStore store(root + "/mail", &c.k.host(), root);
      if (const userland::MailAccount* m = store.get_account(t.account)) c.o << "    mail: " << m->last_sync_status << "\n";
    }
    if (n == 0) c.o << "  none: run `google auth you@gmail.com`\n";
    return;
  }

  if (sub == "sync" || sub == "watch") {
    std::string email = google_pick_account(c, a, oauth);
    bool do_mail = true, do_cal = true, do_con = true, do_notes = true;
    for (size_t i = 2; i < a.size(); ++i) {
      if (a[i] == "mail") { do_cal = do_con = do_notes = false; }
      else if (a[i] == "calendar") { do_mail = do_con = do_notes = false; }
      else if (a[i] == "contacts") { do_mail = do_cal = do_notes = false; }
      else if (a[i] == "notes") { do_mail = do_cal = do_con = false; }
    }
    // `google sync mail calendar`: more than one named part. Rebuild the set from the words given.
    {
      bool named = false;
      for (size_t i = 2; i < a.size(); ++i) if (a[i] == "mail" || a[i] == "calendar" || a[i] == "contacts" || a[i] == "notes") named = true;
      if (named) {
        do_mail = do_cal = do_con = do_notes = false;
        for (size_t i = 2; i < a.size(); ++i) {
          if (a[i] == "mail") do_mail = true;
          if (a[i] == "calendar") do_cal = true;
          if (a[i] == "contacts") do_con = true;
          if (a[i] == "notes") do_notes = true;
        }
      }
    }
    const int every = has_kv(a, "every") ? std::max(30, std::atoi(kv(a, "every").c_str())) : 300;
    for (;;) {
      int failures = 0;
      userland::GoogleSync gs(c.k.host(), root, email);
      if (do_mail) {
        userland::MailStore store(root + "/mail", &c.k.host(), root);
        auto r = store.check_new_mail(store.get_account(email) ? store.get_account(email)->id : "all");
        const userland::MailAccount* m = store.get_account(email);
        const bool ok = r.ok() && m && m->last_sync_status.rfind("Synced", 0) == 0;
        c.o << (ok ? c.o.style(Green()) : c.o.style(Red())) << (ok ? "[mail]" : "[mail FAILED]") << c.o.style(Reset()) << "     "
            << (m ? m->last_sync_status : std::string("no mail account for ") + email + ": run `google auth`") << "\n";
        if (!ok) ++failures;
      }
      auto show = [&](const char* name, Result<userland::GoogleSyncReport> r) {
        if (!r.ok()) {
          c.o << c.o.style(Red()) << "[" << name << " FAILED]" << c.o.style(Reset()) << " " << r.error().message << "\n";
          ++failures;
          return;
        }
        c.o << (r->problems.empty() ? c.o.style(Green()) : c.o.style(Yellow())) << "[" << name << "]" << c.o.style(Reset()) << "  " << r->summary() << "\n";
        for (const auto& p : r->problems) c.o << "    - " << p << "\n";
        if (!r->problems.empty()) ++failures;
      };
      if (do_cal) { userland::CalendarStore cal(root + "/calendar"); show("calendar", gs.sync_calendar(cal)); }
      if (do_con) { userland::ContactsStore con(root + "/contacts"); show("contacts", gs.sync_contacts(con)); }
      if (do_notes) {
        userland::HostBindings hb = userland::surface_bindings(c.k, c.me, "");
        userland::NoteStore ns;
        ns.list = hb.store_list;
        ns.put = hb.store_put;
        show("notes", gs.sync_notes(ns));
      }
      if (sub == "sync") {
        if (failures) fail(Status::IoError, "google sync: " + std::to_string(failures) + " part(s) did not sync");
        return;
      }
      c.o << "  (next sync in " << every << "s; Ctrl-C to stop)\n";
      std::cout.flush();
      std::this_thread::sleep_for(std::chrono::seconds(every));
    }
  }

  if (sub == "probe") {
    // Without signing in or sending any credential: can this machine reach Google, trust its certificates and
    // speak to it? Each line is something that was asked of the real servers just now.
    userland::GoogleSync gs(c.k.host(), root, "probe");
    auto ep = gs.endpoints();
    if (!ep.ok()) fail(ep.error().status, ep.error().message);
    int bad = 0;
    auto line = [&](bool ok, const std::string& what, const std::string& detail) {
      c.o << (ok ? c.o.style(Green()) : c.o.style(Red())) << (ok ? "[ok]  " : "[FAIL]") << c.o.style(Reset()) << " " << what << (detail.empty() ? "" : "  " + detail) << "\n";
      if (!ok) ++bad;
    };
    auto http = [&](const std::string& method, const std::string& url, const std::string& body) -> Result<channel::HttpResponse> {
      auto e = channel::parse_endpoint(url);
      if (!e.ok()) return Result<channel::HttpResponse>(e.error());
      channel::HttpRequest rq;
      rq.method = method;
      rq.endpoint = *e;
      rq.host = &c.k.host();
      rq.kernel_root = root;
      rq.timeout_ms = 20000;
      if (!body.empty()) { rq.body = ByteBuffer(body.begin(), body.end()); rq.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"}); }
      rq.headers.push_back({"Accept", "application/json"});
      return channel::http_send(rq);
    };
    {
      auto r = http("GET", ep->calendar + "/users/me/calendarList", "");
      auto d = r.ok() ? json::parse(r->text()) : Result<json::Value>(Status::IoError);
      line(r.ok() && r->status == 401 && d.ok() && (*d)["error"]["status"].str() == "UNAUTHENTICATED", "Calendar API over HTTPS: refuses an unauthenticated call, as it should",
           r.ok() ? "HTTP " + std::to_string(r->status) : r.error().message);
    }
    {
      auto cfg = oauth.load_config("google");
      const std::string tok = cfg.ok() ? cfg->token_uri : "https://oauth2.googleapis.com/token";
      auto r = http("POST", tok, "grant_type=authorization_code&code=not-a-real-code&client_id=probe&redirect_uri=http%3A%2F%2F127.0.0.1%3A1%2F&client_secret=probe");
      auto d = r.ok() ? json::parse(r->text()) : Result<json::Value>(Status::IoError);
      line(r.ok() && d.ok() && (*d)["error"].is_string(), "OAuth token endpoint: answers a bogus code with an OAuth error", r.ok() && d.ok() ? (*d)["error"].str() : (r.ok() ? "HTTP " + std::to_string(r->status) : r.error().message));
    }
    {
      net::ImapClient imap(c.k.host(), root, 20000);
      auto st = ep->imap_security == "none" ? net::Security::None : ep->imap_security == "starttls" ? net::Security::StartTls : net::Security::Tls;
      auto r = imap.connect(ep->imap_host, ep->imap_port, st);
      line(r.ok(), "IMAP " + ep->imap_host + ":" + std::to_string(ep->imap_port) + " connects and negotiates TLS", r.ok() ? "" : r.error().message);
      if (r.ok()) line(imap.has_capability("AUTH=XOAUTH2"), "IMAP offers AUTH=XOAUTH2", "");
    }
    {
      net::SmtpClient smtp(c.k.host(), root, 20000);
      auto st = ep->smtp_security == "none" ? net::Security::None : ep->smtp_security == "tls" ? net::Security::Tls : net::Security::StartTls;
      auto r = smtp.connect(ep->smtp_host, ep->smtp_port, st);
      line(r.ok(), "SMTP " + ep->smtp_host + ":" + std::to_string(ep->smtp_port) + " connects, upgrades with STARTTLS, EHLO", r.ok() ? "" : r.error().message);
      if (r.ok()) line(smtp.has_mechanism("XOAUTH2"), "SMTP offers AUTH XOAUTH2", "");
    }
    if (bad) fail(Status::IoError, "google probe: " + std::to_string(bad) + " check(s) failed");
    return;
  }

  if (sub == "logout" || sub == "revoke" || sub == "signout") {
    std::string email = google_pick_account(c, a, oauth);
    auto r = oauth.revoke("google", email);
    if (!r.ok()) {
      // forgotten here either way; say what the provider said
      c.o << c.o.style(Yellow()) << "[SIGNED OUT locally]" << c.o.style(Reset()) << " " << r.error().message << "\n";
      return;
    }
    c.o << c.o.style(Green()) << "[SIGNED OUT]" << c.o.style(Reset()) << " " << email << ": the token is revoked at Google and forgotten here.\n"
        << "  Mail, calendar and contacts already synced stay on this machine.\n";
    return;
  }

  fail(Status::Invalid, "google auth [you@gmail.com] [--manual] | auth finish <address> | probe | status | sync [mail|calendar|contacts|notes ...] [account=] | watch [every=300] | logout [account=]");
}

void cmd_contacts(Ctx& c, const Args& a) {
  c.view();
  userland::ContactsStore store(c.k.config().root + "/contacts");
  std::string sub = a.size() > 1 ? a[1] : "list";

  if (sub == "list" || sub == "ls") {
    std::string group = a.size() > 2 ? a[2] : "all";
    auto list = store.contacts_for_group(group);
    c.o << c.o.style(Header()) << "AetherContacts Address Book [" << group << "] (" << list.size() << ")" << c.o.style(Reset()) << "\n";
    for (const auto& card : list) {
      c.o << "  ";
      if (card.is_favorite) c.o << c.o.style(Yellow()) << "*" << c.o.style(Reset());
      else c.o << " ";
      c.o << " [" << card.id << "] " << c.o.style(Bold()) << card.full_name() << c.o.style(Reset())
          << (card.company.empty() ? "" : " (" + card.company + ")")
          << " • " << c.o.style(Cyan()) << card.email << c.o.style(Reset())
          << (card.phone.empty() ? "" : " • " + card.phone) << "\n";
    }
  } else if (sub == "search" || sub == "find") {
    if (a.size() < 3) fail(Status::Invalid, "contacts search <query>");
    auto list = store.search(a[2]);
    c.o << c.o.style(Header()) << "Search Results for '" << a[2] << "' (" << list.size() << ")" << c.o.style(Reset()) << "\n";
    for (const auto& card : list) {
      c.o << "  [" << card.id << "] " << c.o.style(Bold()) << card.full_name() << c.o.style(Reset())
          << " • " << card.email << " • " << card.company << "\n";
    }
  } else if (sub == "show" || sub == "read" || sub == "get") {
    if (a.size() < 3) fail(Status::Invalid, "contacts show <contact-id>");
    const auto* card = store.get_contact(a[2]);
    if (!card) fail(Status::NotFound, "no contact found with id: " + a[2]);
    c.o << c.o.style(Header()) << "Contact Card: " << card->full_name() << c.o.style(Reset()) << "\n";
    c.o << "ID:       " << card->id << "\n";
    c.o << "Name:     " << card->full_name() << "\n";
    if (!card->company.empty())   c.o << "Company:  " << card->company << " (" << card->job_title << ")\n";
    if (!card->email.empty())     c.o << "Email:    " << card->email << " (Home)\n";
    if (!card->email_work.empty())c.o << "Email:    " << card->email_work << " (Work)\n";
    if (!card->phone.empty())     c.o << "Phone:    " << card->phone << " (Mobile)\n";
    if (!card->phone_work.empty())c.o << "Phone:    " << card->phone_work << " (Work)\n";
    if (!card->address.empty())   c.o << "Address:  " << card->address << ", " << card->city << ", " << card->state << " " << card->zip << "\n";
    if (!card->notes.empty())     c.o << "Notes:    " << card->notes << "\n";
    c.o << "vCard:    " << card->vcf_path << "\n";
  } else if (sub == "add" || sub == "new") {
    if (a.size() < 4) fail(Status::Invalid, "contacts add <first_name> <last_name> <email> [phone] [company]");
    userland::ContactCard card;
    card.first_name = a[2];
    card.last_name = a[3];
    card.email = a.size() > 4 ? a[4] : "";
    card.phone = a.size() > 5 ? a[5] : "";
    card.company = a.size() > 6 ? a[6] : "";
    store.add_contact(card);
    c.o << c.o.style(Green()) << "[SAVED]" << c.o.style(Reset()) << " Created contact " << card.full_name() << " (" << card.id << ")\n";
  } else if (sub == "delete" || sub == "rm") {
    if (a.size() < 3) fail(Status::Invalid, "contacts delete <contact-id>");
    if (store.delete_contact(a[2])) {
      c.o << c.o.style(Green()) << "[DELETED]" << c.o.style(Reset()) << " Removed contact " << a[2] << "\n";
    } else {
      fail(Status::NotFound, "no contact found with id: " + a[2]);
    }
  } else if (sub == "favorite" || sub == "fav") {
    if (a.size() < 3) fail(Status::Invalid, "contacts favorite <contact-id>");
    if (store.toggle_favorite(a[2])) {
      c.o << "Toggled favorite for contact " << a[2] << "\n";
    } else {
      fail(Status::NotFound, "no contact found with id: " + a[2]);
    }
  } else if (sub == "export") {
    c.o << store.export_all_vcards();
  } else if (sub == "groups") {
    auto groups = store.get_groups();
    c.o << c.o.style(Header()) << "Contact Groups" << c.o.style(Reset()) << "\n";
    for (const auto& g : groups) {
      c.o << "  [" << g.id << "] " << g.name << " (" << g.count << " cards)\n";
    }
  } else if (sub == "status") {
    c.o << store.status_line() << "\n";
  } else {
    fail(Status::Invalid, "contacts list [group] | search <query> | show <id> | add <first> <last> <email> | delete <id> | favorite <id> | groups | export | status");
  }
}

void cmd_calendar(Ctx& c, const Args& a) {
  c.view();
  userland::CalendarStore store(c.k.config().root + "/calendar");
  std::string sub = a.size() > 1 ? a[1] : "list";

  if (sub == "list" || sub == "ls" || sub == "agenda") {
    auto list = store.all_events();
    c.o << c.o.style(Header()) << "AetherCalendar Schedule (" << list.size() << " events)" << c.o.style(Reset()) << "\n";
    for (const auto& ev : list) {
      c.o << "  [" << ev.id << "] " << c.o.style(Bold()) << ev.date_summary() << " " << ev.time_summary() << c.o.style(Reset()) << "\n"
          << "     " << c.o.style(Cyan()) << ev.title << c.o.style(Reset())
          << (ev.location.empty() ? "" : " @ " + ev.location)
          << " [" << ev.calendar_id << "]\n";
    }
  } else if (sub == "show" || sub == "get") {
    if (a.size() < 3) fail(Status::Invalid, "calendar show <event-id>");
    const auto* ev = store.get_event(a[2]);
    if (!ev) fail(Status::NotFound, "no event found with id: " + a[2]);
    c.o << c.o.style(Header()) << "Calendar Event: " << ev->title << c.o.style(Reset()) << "\n";
    c.o << "ID:          " << ev->id << "\n";
    c.o << "Title:       " << ev->title << "\n";
    c.o << "Date/Time:   " << ev->date_summary() << " • " << ev->time_summary() << "\n";
    if (!ev->location.empty())    c.o << "Location:    " << ev->location << "\n";
    c.o << "Calendar:    " << ev->calendar_id << "\n";
    if (!ev->description.empty()) c.o << "Description: " << ev->description << "\n";
    c.o << "iCalendar:   " << ev->ics_path << "\n";
  } else if (sub == "add" || sub == "new") {
    if (a.size() < 5) fail(Status::Invalid, "calendar add <title> <YYYY-MM-DD> <HH:MM> [duration_mins] [category] [location]");
    userland::CalendarEvent ev;
    ev.title = a[2];
    std::string date_str = a[3];
    std::string time_str = a[4];
    try {
      if (date_str.size() >= 10) {
        ev.year = std::stoi(date_str.substr(0, 4));
        ev.month = std::stoi(date_str.substr(5, 2));
        ev.day = std::stoi(date_str.substr(8, 2));
      }
      if (time_str.size() >= 5) {
        ev.start_hour = std::stoi(time_str.substr(0, 2));
        ev.start_min = std::stoi(time_str.substr(3, 2));
      }
    } catch (...) {}
    if (a.size() > 5) ev.duration_mins = std::atoi(a[5].c_str());
    if (a.size() > 6) ev.calendar_id = a[6];
    if (a.size() > 7) ev.location = a[7];
    store.add_event(ev);
    c.o << c.o.style(Green()) << "[SAVED]" << c.o.style(Reset()) << " Created event: " << ev.title << " (" << ev.id << ")\n";
  } else if (sub == "delete" || sub == "rm") {
    if (a.size() < 3) fail(Status::Invalid, "calendar delete <event-id>");
    if (store.delete_event(a[2])) {
      c.o << c.o.style(Green()) << "[DELETED]" << c.o.style(Reset()) << " Removed event " << a[2] << "\n";
    } else {
      fail(Status::NotFound, "no event found with id: " + a[2]);
    }
  } else if (sub == "export") {
    c.o << store.export_all_ics();
  } else if (sub == "categories" || sub == "cats") {
    auto cats = store.categories();
    c.o << c.o.style(Header()) << "Calendar Categories" << c.o.style(Reset()) << "\n";
    for (const auto& cat : cats) {
      c.o << "  [" << (cat.visible ? "x" : " ") << "] " << cat.name << " (" << cat.id << ")\n";
    }
  } else if (sub == "status") {
    c.o << store.status_line() << "\n";
  } else {
    fail(Status::Invalid, "calendar list | show <id> | add <title> <date> <time> | delete <id> | categories | export | status");
  }
}

void cmd_install(Ctx& c, const Args& a) {
  c.view();
  std::string sub = a.size() > 1 ? a[1] : "status";
  if (sub == "status" || sub == "info") {
    c.o << c.o.style(Header()) << "AIOS Bare-Metal Installation & Dual-Boot Status" << c.o.style(Reset()) << "\n";
#if defined(AIOS_METAL)
    // These three lines used to read "Installed Boot: Active on Hardware",
    // which checked nothing: a machine booted from a live USB with nothing
    // installed anywhere printed it just the same. This kernel cannot see
    // whether it was loaded from a stick or from a disk, and it has no
    // installer, so it says both of those things instead of guessing.
    c.o << "  Substrate       : Bare-Metal x86_64 Kernel\n";
    c.o << "  Running Mode    : Native Hardware (Multiboot2)\n";
    c.o << "  Booted From     : whatever the Multiboot2 loader handed this kernel;\n"
        << "                    a live USB and an installed /boot look identical from here\n";
    c.o << "  Installer       : " << c.o.style(Yellow()) << "NOT PRESENT" << c.o.style(Reset())
        << " -- this kernel cannot install itself to a disk.\n"
        << "                    Install from a hosted Linux session: install dualboot\n";
#else
    c.o << "  Substrate       : Hosted Linux Kernel\n";
    struct stat st;
    bool kernel_exists = (stat("/boot/aios-metal.elf", &st) == 0);
    c.o << "  /boot Kernel    : ";
    if (kernel_exists) {
      // INSTALLED is not the whole truth, and the interesting half was missing.
      // /boot/aios-metal.elf is a COPY: rebuilding the kernel does not update
      // it, so this said INSTALLED in green while the GRUB entry pointed at a
      // kernel a day older than the build sitting in the tree — reboot and you
      // get yesterday's machine with no way to tell from here.
      struct stat built;
      // The build to compare with: build/metal/ under the working directory,
      // else metal/ beside the running binary (which is in build/).
      std::string built_path = "build/metal/aios-metal.elf";
      if (stat(built_path.c_str(), &built) != 0) built_path = beside_binary(c, "metal/aios-metal.elf");
      const bool have_build = (stat(built_path.c_str(), &built) == 0);
      const bool stale = have_build && (built.st_size != st.st_size || built.st_mtime > st.st_mtime);
      c.o << c.o.style(stale ? Yellow() : Green())
          << "INSTALLED (/boot/aios-metal.elf, " << fmt_bytes(st.st_size) << ")"
          << c.o.style(Reset());
      if (stale) {
        c.o << c.o.style(Yellow()) << "  STALE" << c.o.style(Reset())
            << " — the build at " << built_path << " is " << fmt_bytes(built.st_size)
            << "; `install dualboot` re-copies it\n";
      } else if (have_build) {
        c.o << "  matches the build at " << built_path << "\n";
      } else {
        c.o << "  (no build in this tree to compare against)\n";
      }
      // The UEFI copy is the one a machine that boots UEFI actually loads, and
      // it is a SECOND copy that goes stale on its own.
      struct stat efi;
      if (stat("/boot/efi/EFI/AIOS/aios-metal.elf", &efi) == 0) {
        const bool efi_stale = have_build && (built.st_size != efi.st_size || built.st_mtime > efi.st_mtime);
        c.o << "  UEFI copy       : " << (!have_build ? "PRESENT (no build to compare against)" : efi_stale ? "STALE" : "current")
            << " (/boot/efi/EFI/AIOS/aios-metal.elf, " << fmt_bytes(efi.st_size) << ")\n";
      } else {
        c.o << "  UEFI copy       : ABSENT — a machine that boots UEFI will not find it\n";
      }
    } else {
      c.o << c.o.style(Red()) << "NOT INSTALLED" << c.o.style(Reset()) << "\n";
    }
    bool grub_exists = (stat("/etc/grub.d/42_aios", &st) == 0);
    c.o << "  GRUB Dual-Boot  : ";
    if (grub_exists) {
      c.o << c.o.style(Green()) << "CONFIGURED (/etc/grub.d/42_aios active)" << c.o.style(Reset()) << "\n";
    } else {
      c.o << c.o.style(Yellow()) << "NOT CONFIGURED" << c.o.style(Reset()) << "\n";
    }
    c.o << "\n" << c.o.style(Bold()) << "Dual-Boot & Installation Commands:" << c.o.style(Reset()) << "\n";
    c.o << "  " << c.o.style(Cyan()) << W{26} << "install dualboot" << c.o.style(Reset()) << "Install AIOS to /boot & configure host GRUB menu for dual boot\n";
    c.o << "  " << c.o.style(Cyan()) << W{26} << "install disk <dev>" << c.o.style(Reset()) << "Flash bootable standalone AIOS image to target disk (e.g. /dev/sdc)\n";
    c.o << "  " << c.o.style(Cyan()) << W{26} << "install remove" << c.o.style(Reset()) << "Remove AIOS dual-boot entries from host GRUB\n";
#endif
  } else if (sub == "dualboot" || sub == "host" || sub == "here") {
    need(c.me.operate());
#if defined(AIOS_METAL)
    c.o << "[INSTALL] Running natively on bare metal, which is not the same as being\n"
        << "          installed: there is no installer in this kernel. Boot the hosted\n"
        << "          Linux side and run `install dualboot` there.\n";
#else
    c.o << c.o.style(Cyan()) << "[INSTALL DUAL-BOOT]" << c.o.style(Reset()) << " Installing AIOS bare-metal kernel to host bootloader...\n";
    c.o.flush();
    int res = system((install_script() + " dualboot").c_str());
    if (res == 0) {
      c.o << c.o.style(Green()) << "[INSTALL SUCCESS]" << c.o.style(Reset()) << " AIOS bare-metal is installed and ready for dual boot!\n";
    } else {
      c.o << c.o.style(Red()) << "[INSTALL FAILED]" << c.o.style(Reset()) << " Error running install-dualboot.sh (exit code " << res << ")\n";
    }
#endif
  } else if (sub == "disk" || sub == "target" || sub == "flash") {
    if (a.size() < 3) fail(Status::Invalid, "install disk <block_device> (e.g. install disk /dev/sdc)");
    need(c.me.operate());
#if defined(AIOS_METAL)
    c.o << "[FLASH DISK] Direct block device flash requires hosted tool.\n";
#else
    std::string dev = a[2];
    // A block device path and nothing else: it goes to the installer as one
    // argument (no shell), so `install disk "/dev/x;cmd"` cannot run cmd.
    bool plain = dev.rfind("/dev/", 0) == 0 && dev.size() > 5 && dev.find("..") == std::string::npos;
    for (char ch : dev) if (!(std::isalnum((unsigned char)ch) || ch == '/' || ch == '-' || ch == '_' || ch == '.' || ch == ':')) plain = false;
    if (!plain) fail(Status::Invalid, "install disk takes a block device path under /dev/ (letters, digits, / - _ . :), got '" + dev + "'");
    c.o << c.o.style(Yellow()) << "[FLASH DISK]" << c.o.style(Reset()) << " Flashing AIOS to " << dev << "...\n";
    c.o.flush();
    const std::string script = install_script();
    int res = -1;
    pid_t pid = fork();
    if (pid == 0) { execl("/bin/sh", "sh", script.c_str(), "disk", dev.c_str(), (char*)nullptr); _exit(127); }
    if (pid > 0) { int st = 0; waitpid(pid, &st, 0); res = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0); }
    if (res == 0) {
      c.o << c.o.style(Green()) << "[FLASH SUCCESS]" << c.o.style(Reset()) << " Disk " << dev << " is now bootable!\n";
    } else {
      fail(Status::IoError, "flashing " + dev + " failed: " + script + " exited with status " + std::to_string(res));
    }
#endif
  } else if (sub == "remove" || sub == "uninstall") {
    need(c.me.operate());
#if defined(AIOS_METAL)
    c.o << "[UNINSTALL] Cannot uninstall host bootloader from within bare-metal guest.\n";
#else
    c.o << c.o.style(Cyan()) << "[UNINSTALL]" << c.o.style(Reset()) << " Removing AIOS dual-boot configuration...\n";
    c.o.flush();
    int res = system((install_script() + " uninstall").c_str());
    if (res == 0) {
      c.o << c.o.style(Green()) << "[UNINSTALL SUCCESS]" << c.o.style(Reset()) << " AIOS dual boot removed cleanly.\n";
    }
#endif
  } else {
    fail(Status::Invalid, "install [status] | install dualboot | install disk <dev> | install remove");
  }
}

// The machine's own bus, and whether two things on it can reach each other.
void cmd_bus(Ctx& c, const Args& a) {
  compute::P2PAcceleratorDMA dma;
  std::string sub = a.size() > 1 ? a[1] : "list";

  if (sub == "list") {
    auto fns = need(dma.enumerate());
    c.view();
    c.o << c.o.style(Header()) << W{16} << "SLOT" << W{12} << "KIND" << W{12} << "VENDOR:DEV"
        << W{14} << "DRIVER" << W{14} << "UPSTREAM" << "WINDOWS" << c.o.style(Reset()) << "\n";
    for (const auto& f : fns) {
      char id[16];
      std::snprintf(id, sizeof id, "%04x:%04x", f.vendor, f.device);
      std::string windows;
      for (const auto& b : f.bars) {
        if (!b.memory) continue;
        char w[64];
        std::snprintf(w, sizeof w, "bar%u@0x%llx/%s%s ", (unsigned)b.index,
                      (unsigned long long)b.address, fmt_bytes(b.size).c_str(),
                      b.prefetchable ? "p" : "");
        windows += w;
      }
      c.o << W{16} << f.slot << W{12} << f.kind << W{12} << id
          << W{14} << (f.driver.empty() ? "-" : f.driver)
          << W{14} << (f.upstream.empty() ? "root" : f.upstream)
          << (windows.empty() ? "-" : windows) << "\n";
    }
    return;
  }
  if (sub == "peer") {
    if (a.size() < 4) fail(Status::Invalid, "bus peer <src-slot> <dst-slot> [bytes]");
    uint64_t bytes = a.size() > 4 ? to_u64(a[4]) : 4096;
    auto m = need(dma.map_p2p_region(a[2], a[3], bytes));
    c.view();
    char sa[32], ta[32];
    std::snprintf(sa, sizeof sa, "0x%llx", (unsigned long long)m.source_bar_addr);
    std::snprintf(ta, sizeof ta, "0x%llx", (unsigned long long)m.target_bar_addr);
    c.o << "  " << W{18} << "Source" << ": " << m.source_slot << " answers at " << sa
        << " (" << fmt_bytes(m.source_bar_size) << ")\n";
    c.o << "  " << W{18} << "Target" << ": " << m.target_slot << " answers at " << ta
        << " (" << fmt_bytes(m.target_bar_size) << ")\n";
    c.o << "  " << W{18} << "Fabric" << ": " << m.path << "\n";
    c.o << "  " << W{18} << "Peer capable" << ": " << c.o.style(m.active ? Green() : Yellow())
        << (m.active ? "yes" : "no") << c.o.style(Reset()) << "\n";
    auto moved = dma.transfer_direct(m);
    c.o << "  " << W{18} << "Transfer" << ": "
        << (moved.ok() ? std::to_string(*moved) + " bytes" : moved.error().message) << "\n";
    return;
  }
  fail(Status::Invalid, "bus list | bus peer <src-slot> <dst-slot> [bytes]");
}

// Bytes as hex. The command surface's stream has no std::hex and no
// const char* overload, so a ternary picking "0" decays to bool and prints
// "true" — which is exactly what the first attempt at this did.
static std::string hex_bytes(const uint8_t* p, size_t n) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) { out += digits[p[i] >> 4]; out += digits[p[i] & 0x0F]; }
  return out;
}

// The machine's root of trust, exercised. Every number this prints came off a
// TPM over the wire; there is nothing here to print on a machine without one.
void cmd_attest(Ctx& c, const Args& a) {
  std::string sub = a.size() > 1 ? a[1] : "status";
  capability::HardwareAttestor att;

  if (sub == "status") {
    c.view();
    c.o << "  " << W{18} << "Root of trust" << ": " << att.describe_root_of_trust() << "\n";
    if (!att.available()) return;
    auto rnd = att.random(16);
    c.o << "  " << W{18} << "TPM_GetRandom" << ": ";
    if (!rnd.ok()) c.o << rnd.error().str() << "\n";
    else c.o << hex_bytes(rnd->data(), rnd->size()) << "\n";
    return;
  }
  if (sub == "pcrs") {
    need(att.attach());
    std::vector<uint32_t> want;
    for (size_t i = 2; i < a.size(); ++i) want.push_back((uint32_t)to_u64(a[i]));
    if (want.empty()) want = {0, 1, 2, 3, 4, 5, 6, 7};
    auto pcrs = need(att.read_pcrs(want));
    c.view();
    c.o << c.o.style(Header()) << "  PCR  SHA-256 (as the TPM holds it now)" << c.o.style(Reset()) << "\n";
    for (const auto& p : pcrs) {
      c.o << "  " << W{5} << p.index << hex_bytes(p.digest.data(), p.digest.size()) << "\n";
    }
    return;
  }
  if (sub == "quote") {
    need(att.attach());
    std::vector<uint32_t> want;
    std::string nonce_text = kv(a, "nonce", "");
    for (size_t i = 2; i < a.size(); ++i) {
      if (a[i].rfind("nonce=", 0) == 0) continue;
      want.push_back((uint32_t)to_u64(a[i]));
    }
    if (want.empty()) want = {0, 1, 2, 3, 7};
    // Freshness. Without it a quote proves the machine was in that state once,
    // not that it is now, and a replayed one would check out.
    std::vector<uint8_t> nonce;
    if (!nonce_text.empty()) nonce.assign(nonce_text.begin(), nonce_text.end());
    else {
      nonce.resize(20);
      c.k.host().random(MutableByteSpan(nonce.data(), nonce.size()));
    }
    c.o << "  asking the TPM for a quote over " << want.size() << " PCR(s)...\n";
    c.o.flush();
    auto q = need(att.generate_tpm2_quote(want, nonce));
    c.view();
    c.o << "  " << W{18} << "Attestation" << ": " << q.attested.size() << " bytes, TPM-generated, signed\n";
    c.o << "  " << W{18} << "Signed by" << ": ECDSA P-256 attestation key created in the endorsement hierarchy\n";
    c.o << "  " << W{18} << "Key" << ": ak=" << crypto::bytes_to_hex(ByteSpan(q.signer_public.data(), q.signer_public.size()))
        << "   (the value a fleet members file lists for this machine)\n";
    c.o << "  " << W{18} << "Signature" << ": ";
    c.o << hex_bytes(q.signature.data(), std::min<size_t>(8, q.signature.size()))
        << "... (" << q.signature.size() << " bytes, r||s)\n";
    c.o << "  " << W{18} << "TPM clock" << ": " << q.clock << " ms\n";
    c.o << "  " << W{18} << "PCRs covered" << ": ";
    for (const auto& p : q.pcrs) c.o << p.index << " ";
    c.o << "\n";
    c.o << "  " << W{18} << "Nonce echoed" << ": " << (q.nonce == nonce ? "yes" : "NO — this quote is not fresh") << "\n";
    c.o << "  " << W{18} << "Verified" << ": " << c.o.style(q.verified ? Green() : Red())
        << (q.verified ? "yes — the signature checks against the key, and the PCR values hash to the signed digest"
                       : "NO")
        << c.o.style(Reset()) << "\n";
    // And prove the check can fail: a quote whose PCR values have been edited
    // must not verify, or the verifier is not checking anything.
    if (!q.pcrs.empty()) {
      auto tampered = q;
      tampered.pcrs[0].digest[0] ^= 0x01;
      auto again = att.verify_quote(tampered, nonce);
      bool still_ok = again.ok() && *again;
      c.o << "  " << W{18} << "Tamper check" << ": one bit flipped in PCR " << tampered.pcrs[0].index << " -> "
          << c.o.style(still_ok ? Red() : Green())
          << (still_ok ? std::string("still verified, which is wrong") : "rejected: " + again.error().message)
          << c.o.style(Reset()) << "\n";
    }
    return;
  }
  fail(Status::Invalid, "attest status | attest pcrs [n...] | attest quote [n...] [nonce=<text>]");
}

// What this machine can actually do, asked of the machine.
//
// This was a list of twenty-six hand-typed lines, most of them reading
// "[x] Complete (aios::SomeClass)". Every one of them was a claim a person had
// written down, and several were false the day they were written: TASK-002
// said Complete over an in-memory std::vector, TASK-004 over CPU for-loops,
// TASK-008 over signatures made by XORing bytes with a loop index. A status
// surface that cannot be wrong about anything is a status surface that reports
// nothing, so nothing here is typed — every line below is the answer to a
// question put to the running kernel, and a subsystem that cannot be
// interrogated does not appear.
void cmd_subsystems(Ctx& c, const Args& a) {
  c.view();
  const bool verbose = a.size() > 1 && (a[1] == "-v" || a[1] == "full");

  auto row = [&](const char* what, bool real, const std::string& detail) {
    c.o << "  " << (real ? c.o.style(Green()) : c.o.style(Yellow()))
        << W{4} << (real ? "yes" : "no") << c.o.style(Reset())
        << W{26} << what << detail << "\n";
  };

  c.o << c.o.style(Header()) << "What this machine can do, asked of the machine" << c.o.style(Reset()) << "\n";

  // Compute. Which devices are in the fabric, and what each one really is.
  {
    auto devs = c.k.devices().all();
    size_t gpus = 0;
    std::string names;
    for (auto* d : devs) {
      if (d->type() == DeviceType::GPU) gpus++;
      std::string dev_detail = d->name();
      auto mem = d->memory_stats();
      if (mem.ok() && mem->total_bytes > 0) {
        dev_detail += " [" + fmt_bytes(mem->used_bytes) + "/" + fmt_bytes(mem->total_bytes) + " loaded]";
      }
      names += (names.empty() ? "" : ", ") + dev_detail;
    }
    row("compute devices", devs.size() > 1,
        std::to_string(devs.size()) + " in the fabric (" + std::to_string(gpus) + " gpu): " + names);
    const auto& acc = c.k.accelerators();
    if (!acc.note.empty()) c.o << "                                " << acc.note << "\n";
  }

  // Storage: what the substrate says it has, and whether a real block driver
  // is bound underneath the kernel's volume.
  {
    auto vols = c.k.host().storage(c.k.config().root);
    std::string detail = vols.ok() ? std::to_string(vols->size()) + " volume(s)" : vols.error().message;
    if (vols.ok() && verbose) {
      for (const auto& v : *vols) detail += "\n                                " + v.name + "  " + v.kind + "  " + fmt_bytes(v.capacity);
    }
    row("storage", vols.ok() && !vols->empty(), detail);
  }

  // The buses. A machine that cannot enumerate its own bus cannot claim a
  // driver for anything on it.
  {
    auto hw = c.k.host().hardware();
    size_t n = hw.ok() ? hw->size() : 0;
    std::string detail = hw.ok() ? std::to_string(n) + " function(s) on the bus" : hw.error().message;
    if (hw.ok() && verbose) {
      for (const auto& d : *hw) detail += "\n                                " + d.slot + "  " + d.kind + "  " + d.name + (d.driver.empty() ? "" : "  [" + d.driver + "]");
    }
    row("bus enumeration", hw.ok() && n > 0, detail);
  }

  // Network interfaces, from the substrate.
  {
    auto nets = c.k.host().network();
    size_t up = 0;
    if (nets.ok()) for (const auto& n : *nets) if (n.up && !n.loopback) up++;
    row("network interfaces", nets.ok() && up > 0,
        nets.ok() ? std::to_string(up) + " up, " + std::to_string(nets->size()) + " total"
                  : nets.error().message);
  }

  // Sound and speech.
  {
    auto voices = hal::speech_voices();
    row("speech synthesis", voices.ok() && !voices->empty(),
        voices.ok() ? std::to_string(voices->size()) + " voice(s) from the substrate's engine" : voices.error().message);
    auto rec = hal::speech_recognizer();
    row("speech recognition", rec.ok(), rec.ok() ? *rec : rec.error().message);
    auto ends = hal::audio_endpoints();
    size_t ins = 0, outs = 0;
    if (ends.ok()) for (const auto& e : *ends) (e.input ? ins : outs)++;
    row("audio hardware", ends.ok() && (ins || outs),
        ends.ok() ? std::to_string(ins) + " in, " + std::to_string(outs) + " out" : ends.error().message);
  }

  // Scheduling: quotas are enforced by the scheduler, so ask it.
  {
    auto quotas = c.k.quotas().list();
    row("execution quotas", true,
        std::to_string(quotas.size()) + " entity(ies) bounded; the scheduler charges measured device time");
  }

  // Services: real supervised processes, so count the ones actually running.
  {
    auto& mgr = daemons_for(c.k);
    (void)mgr.supervise();
    auto svc = mgr.list_services();
    size_t running = 0;
    for (const auto& u : svc) if (u.status == userland::ServiceStatus::Running) running++;
    row("service manager", true,
        std::to_string(running) + " running of " + std::to_string(svc.size()) + " registered (real processes)");
  }

  // Attestation: is there a root of trust on this machine at all?
  {
    capability::HardwareAttestor att;
    auto root = att.describe_root_of_trust();
    row("hardware attestation", att.available(), root);
  }

  c.o << "\n  " << c.o.style(Dim())
      << "Nothing above is a stored claim; each line is what the kernel answered just now."
      << c.o.style(Reset()) << "\n";
  if (!verbose) c.o << "  " << c.o.style(Dim()) << "`subsystems -v` lists the devices behind each answer." << c.o.style(Reset()) << "\n";
}

Result<void> run_line(Session& s, const std::string& raw) {
  g_raw_line = raw;
  std::string line = raw;
  // trailing "=> NAME" binds the produced identity
  std::string bind;
  auto arrow = line.rfind("=>");
  if (arrow != std::string::npos) { bind = line.substr(arrow + 2); line = line.substr(0, arrow); bind.erase(0, bind.find_first_not_of(" \t")); bind.erase(bind.find_last_not_of(" \t\r\n") + 1); }
  bool expect_fail = false;
  size_t start = line.find_first_not_of(" \t");
  if (start != std::string::npos && line[start] == '!') { expect_fail = true; line[start] = ' '; }
  auto a = tokenize(line);
  if (a.empty()) return Ok();
  s.commands++;
  if (!raw.empty() && raw[0] != ' ') { auto& h = s.vars["_history"]; h += (h.empty() ? "" : "\n") + line; }
  if (!s.me) return Result<void>(Status::Denied, "this console is bound to no entity; the command surface acts only as one");
  Ctx c{s, *s.out, Resolver{s}, *s.kernel, *s.me};
  // The run directory, from THIS machine's root, on every line. It used to be
  // filled in only by the hosted-actuation installer — which returns early when
  // the substrate already owns the hook — and its derivation did nothing at all
  // for a root with no '/' in it. So the fallback stood, and every scene the
  // console composed went to one particular host's absolute path.
  g_act_run_dir = userland::run_dir(c.k);
  Result<void> outcome = Ok();
  try {
    const std::string& cmd = a[0];
    if (cmd == "help") {
      if (a.size() > 1) {
        const std::string& target = a[1];
        if (target == "git") { cmd_git(c, {"git", "help"}); }
        else if (target == "ssh") { cmd_ssh(c, {"ssh", "help"}); }
        else if (target == "tx") { cmd_tx(c, {"tx", "help"}); }
        else if (target == "webserver" || target == "web") {
          c.o << c.o.style(Header()) << "AIOS Web Server (aios webserver)" << c.o.style(Reset()) << "\n\n"
              << "Usage:\n"
              << "  aios webserver <config-file>    Start vhost HTTP(S) server with FastCGI PHP-FPM\n"
              << "  aios webserver --help           Show vhost configuration format and examples\n";
        } else {
          std::string help_text;
          { Out h([&](const std::string& t) { help_text += t; }); print_help(h); }
          bool found = false;
          for (auto& help_line : split(help_text, '\n')) {
            std::string clean;
            for (size_t i = 0; i < help_line.size(); ++i) {
              if (help_line[i] == '\033') { while (i < help_line.size() && help_line[i] != 'm') ++i; continue; }
              clean.push_back(help_line[i]);
            }
            std::string t = clean;
            t.erase(0, t.find_first_not_of(" \t"));
            std::string cmd_name = t.substr(0, t.find_first_of(" \t|"));
            if (cmd_name == target || t.find(target) != std::string::npos) {
              c.o << line << "\n";
              found = true;
            }
          }
          if (!found) {
            c.o << "No specific help for '" << target << "'. Type `help` to list all commands.\n";
          }
        }
      } else {
        print_help(c.o);
      }
    }
    else if (cmd == "authority") cmd_authority(c, a);
    else if (cmd == "ask") {
      // ask [mind=<resident>] [cycles=N] <request...>
      std::string who = kv(a, "mind", "mind"); uint64_t cycles = has_kv(a, "cycles") ? to_u64(kv(a, "cycles")) : 12;
      std::string req; for (size_t i = 1; i < a.size(); ++i) { if (a[i].rfind("mind=", 0) == 0 || a[i].rfind("cycles=", 0) == 0 || a[i].rfind("show=", 0) == 0) continue; req += (req.empty() ? "" : " ") + c.r.expand(a[i]); }
      if (req.empty()) fail(Status::Invalid, "ask [show=1] <what you want> — the resident mind acts on the machine as itself and answers; show=1 shows its working");
      const bool was_verbose = s.verbose;
      if (has_kv(a, "show")) s.verbose = kv(a, "show") != "0";
      auto r = ask(s, req, who, cycles);
      s.verbose = was_verbose;
      need(r);
    }
    else if (cmd == "dream" || cmd == "dreams") need(dream_command(s, a));
    else if (cmd == "organ" || cmd == "organs") need(organ_command(s, a));
    else if (cmd == "face" || cmd == "faces") need(face_command(s, a));
    else if (cmd == "whoami" || cmd == "id") { const Entity* e = c.k.entities().get(c.me.self()); c.o << "you are entity " << c.me.self().str() << (e ? " (" + e->label + ")" : "") << "; the kernel " << c.k.self().short_str() << " is another entity\n"; }
    else if (cmd == "info") cmd_info(c, a);
    else if (cmd == "sysstat") cmd_sysstat(c, a);
    else if (cmd == "daemon") cmd_daemon(c, a);
    else if (cmd == "keys" || cmd == "keybindings" || cmd == "keymap") cmd_keys(c, a);
    else if (cmd == "app" || cmd == "apps") cmd_app(c, a);
    else if (cmd == "serve") cmd_serve(c, a);
    else if (cmd == "js" || cmd == "javascript") cmd_js(c, a);
    else if (cmd == "miner" || cmd == "mine") cmd_miner(c, a);
    else if (cmd == "btc" || cmd == "bitcoin") cmd_chain(c, a);   // the Bitcoin chain client, not the hashing exercise
    else if (cmd == "wallet") cmd_wallet(c, a);
    else if (cmd == "git") cmd_git(c, a);
    else if (cmd == "npm") cmd_npm(c, a);
    else if (cmd == "pip" || cmd == "pip3" || cmd == "python") cmd_pip(c, a);
    else if (cmd == "cargo" || cmd == "crates") cmd_cargo(c, a);
    else if (cmd == "go" || cmd == "gomod") cmd_go(c, a);
    else if (cmd == "elf") cmd_elf(c, a);
    else if (cmd == "sense" || cmd == "look") cmd_sense(c, a);
    else if (cmd == "directive" || cmd == "directives") cmd_directive(c, a);
    else if (cmd == "horizon") cmd_horizon(c, a);
    else if (cmd == "identity") cmd_identity(c, a);
    else if (cmd == "ssh") cmd_ssh(c, a);
    else if (cmd == "chain") cmd_chain(c, a);
    else if (cmd == "contract" || cmd == "solve") cmd_contract(c, a);
    else if (cmd == "task" || cmd == "taskledger") cmd_task(c, a);
    else if (cmd == "voice" || cmd == "speech" || cmd == "talk" || cmd == "listen") cmd_voice(c, a);
    else if (cmd == "sql" || cmd == "asql" || cmd == "mariadb" || cmd == "db") cmd_sql(c, a);
    else if (cmd == "mail" || cmd == "email" || cmd == "aethermail") cmd_mail(c, a);
    else if (cmd == "google" || cmd == "gmail") cmd_google(c, a);
    else if (cmd == "contacts" || cmd == "addressbook" || cmd == "aethercontacts") cmd_contacts(c, a);
    else if (cmd == "calendar" || cmd == "cal" || cmd == "agenda" || cmd == "aethercalendar") cmd_calendar(c, a);
    else if (cmd == "ingress" || cmd == "receive" || cmd == "upload") cmd_ingress(c, a);
    else if (cmd == "subsystems" || cmd == "tasks") cmd_subsystems(c, a);
    else if (cmd == "attest" || cmd == "tpm") cmd_attest(c, a);
    else if (cmd == "bus" || cmd == "pci") cmd_bus(c, a);
    else if (cmd == "gui" || cmd == "ui" || cmd == "console" || cmd == "desktop") cmd_gui(c, a);
    else if (cmd == "browser" || cmd == "unblink") cmd_browser(c, a);
    else if (cmd == "devices") cmd_devices(c, a);
    else if (cmd == "storage") cmd_storage(c, a);
    else if (cmd == "network") cmd_network(c, a);
    else if (cmd == "entity") cmd_entity(c, a);
    else if (cmd == "cap") cmd_cap(c, a);
    else if (cmd == "grain") cmd_grain(c, a);
    else if (cmd == "sat") need(run_sat_command(c.k, c.me, c.o, a));
    else if (cmd == "mem") cmd_mem(c, a);
    else if (cmd == "state") cmd_state(c, a);
    else if (cmd == "asqlcap") cmd_asqlcap(c, a);
    else if (cmd == "kv") cmd_kv(c, a);
    else if (cmd == "tensor") cmd_tensor(c, a);
    else if (cmd == "graph") cmd_graph(c, a);
    else if (cmd == "compute") cmd_compute(c, a);
    else if (cmd == "infer") cmd_infer(c, a);
    else if (cmd == "quota") cmd_quota(c, a);
    else if (cmd == "channel") cmd_channel(c, a);
    else if (cmd == "checkpoint") cmd_checkpoint(c, a);
    else if (cmd == "restore") cmd_restore(c, a);
    else if (cmd == "rewind") cmd_rewind(c, a);
    else if (cmd == "mind") cmd_mind(c, a);
    else if (cmd == "qwen" || cmd == "llm" || cmd == "spark") cmd_qwen(c, a);
    else if (cmd == "jepa" || cmd == "world-model") cmd_jepa(c, a);
    else if (cmd == "lm" || cmd == "language") cmd_lm(c, a);
    else if (cmd == "display" || cmd == "screen" || cmd == "show") cmd_display(c, a);
    else if (cmd == "net" && a.size() > 1 && (a[1] == "check" || a[1] == "repair" || a[1] == "correct")) cmd_net_correct(c, a);
    else if (cmd == "store") cmd_store(c, a);
    else if (cmd == "expect") cmd_expect(c, a);
    else if (cmd == "expect-error") cmd_expect_error(c, a);
    else if (cmd == "node") cmd_node(c, a);
    else if (cmd == "p2p" || cmd == "dht" || cmd == "mesh") cmd_p2p(c, a);
    else if (cmd == "fleet") cmd_fleet(c, a);
    else if (cmd == "gateway") cmd_gateway(c, a);
    else if (cmd == "feed" || cmd == "rss") cmd_feed(c, a);
    else if (cmd == "tx" || cmd == "transaction") cmd_tx(c, a);
    else if (cmd == "watchdog") cmd_watchdog(c, a);
    else if (cmd == "sleep") { if (a.size() < 2) fail(Status::Invalid, "sleep <seconds>"); hal::sleep_ms((uint32_t)(to_f64(a[1]) * 1000)); }
    else if (cmd == "build") {
      need(c.me.operate());
      if (!s.host) fail(Status::Unsupported, "no host tool bound to this session");
      std::vector<std::string> argv;
      if (a.size() < 2) {
        argv = {"cmake", "--build", "build"};
      } else if (a[1] == "test") {
        argv = {"scripts/drive.sh"};
      } else if (a[1] == "gate") {
        argv = {"sh", "-c", "scripts/harness-smell.sh && scripts/zero-host-audit.sh --gate && scripts/authority-gate.sh"};
      } else {
        for (size_t i = 1; i < a.size(); ++i) argv.push_back(c.r.expand(a[i]));
      }
      c.o.flush();
      need(s.host(argv, c.o, c.me));
    }
    else if (cmd == "install" || cmd == "dualboot") cmd_install(c, a);
    // `host load correct` is this kernel's own act, not the substrate tool's:
    // it measures, sets a quota through the same action vector the policy head
    // uses, and measures again.
    else if (cmd == "host" && a.size() > 2 && a[1] == "load") cmd_load_correct(c, a);
    else if (cmd == "load") cmd_load_correct(c, a);
    else if (cmd == "host") {
      // The hardware host's bootstrap (disks, install, reboot...) when the substrate provides one. Not a kernel operation.
      if (!s.host) fail(Status::Unsupported, "no host tool bound to this session");
      // Operating the machine beneath the kernel is kernel:execute; the
      // substrate's tool asks for kernel:destroy itself when a verb would wipe
      // the volume the kernel lives on.
      need(c.me.operate());
      std::vector<std::string> argv;
      for (size_t i = 1; i < a.size(); ++i) argv.push_back(c.r.expand(a[i]));
      c.o.flush();
      need(s.host(argv, c.o, c.me));
    }
    else if (cmd == "reboot" || cmd == "restart") {
      need(c.me.operate());
      c.o << "Initiating system reboot...\n";
      c.o.flush();
      if (s.host) {
        (void)s.host({"reboot"}, c.o, c.me);
      }
#if defined(AIOS_METAL)
      metal_power_reboot();
#else
      sync();
      const std::string why = ask_substrate_for_power(false);
      sync();
      if (!why.empty()) {
        // NOT std::exit. Exiting here is what made a refused reboot look like
        // a completed one: the supervisor restarts this process and the
        // desktop reappears as though the machine had come back up.
        fail(Status::Denied, "this machine may not reboot itself: " + why);
      }
      // Accepted: the substrate is taking the machine down and nothing below
      // this line will run.
#endif
    }
    else if (cmd == "scene" || cmd == "animate") {
      // The desktop compositor already polls $AOS_RUN/scene four times a
      // second and switches what it paints; nothing could write it. The ask
      // briefing advertised `scene` as a command for long enough that the mind
      // kept trying it and getting "unknown command: scene".
      static const struct { const char* word; const char* canon; const char* what; } kScenes[] = {
        {"beach", "beach", "moonlit ocean and beach"},
        {"ocean", "beach", "moonlit ocean and beach"},
        {"sea",   "beach", "moonlit ocean and beach"},
        {"cyber", "cyber", "neural matrix with a perspective grid"},
        {"neural","cyber", "neural matrix with a perspective grid"},
        {"orb",   "orb",   "quantum plasma orb"},
        {"quantum","orb",  "quantum plasma orb"},
      };
      // The one run directory the compositor reads ($AOS_RUN, else <root>/run),
      // not one particular host's /home/arch/aos/run.
      const std::string run = userland::run_dir(c.k);
      (void)c.k.host().make_dir(run);   // host-io: the machine's own run dir (scene file)
      const std::string path = run + "/scene";
      if (a.size() < 2) {
        auto cur = c.k.host().read_file(path);   // host-io: the machine's own run dir (scene file)
        std::string now_val = cur.ok() ? std::string((const char*)cur->data(), cur->size()) : std::string();
        while (!now_val.empty() && (now_val.back() == '\n' || now_val.back() == ' ')) now_val.pop_back();
        c.o << "desktop scene: " << (now_val.empty() ? "beach (default)" : now_val) << "\n";
        c.o << "set one with: scene beach|ocean | scene cyber | scene orb\n";
        c.o << "the running desktop picks it up within half a second; it is read from "
            << path << "\n";
        return Ok();
      }
      std::string want = a[1];
      for (auto& ch : want) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
      const char* canon = nullptr; const char* what = nullptr;
      for (const auto& sc : kScenes) if (want == sc.word) { canon = sc.canon; what = sc.what; }
      if (!canon) fail(Status::Invalid, "scene names: beach (ocean), cyber (neural), orb (quantum)");
      need(c.me.operate());
      std::string body = std::string(canon) + "\n";
      auto wr = c.k.host().write_file(path, ByteSpan((const uint8_t*)body.data(), body.size()));   // host-io: the machine's own run dir (scene file)
      if (!wr.ok()) fail(Status::IoError, "could not write " + path + ": " + wr.error().str());
      c.o << "desktop scene set to " << canon << " (" << what << ")\n";
      c.o << "a desktop already on the display switches to it within half a second\n";
    }
    else if (cmd == "poweroff" || cmd == "shutdown" || cmd == "halt") {
      need(c.me.operate());
      c.o << "Initiating system shutdown...\n";
      c.o.flush();
      if (s.host) {
        (void)s.host({"poweroff"}, c.o, c.me);
      }
#if defined(AIOS_METAL)
      metal_power_off();
#else
      sync();
      const std::string why = ask_substrate_for_power(true);
      sync();
      if (!why.empty()) {
        fail(Status::Denied, "this machine may not power itself off: " + why);
      }
#endif
    }
    else if (cmd == "set") { if (a.size() < 2) fail(Status::Invalid, "set NAME"); s.vars[a[1]] = s.last; c.o << "$" << a[1] << " = " << s.last << "\n"; }
    else if (cmd == "echo") {
      bool redir = false; for (auto& t : a) if (t == ">" || t == ">>") redir = true;
      if (redir) { shell_command(s, *s.out, *s.me, a); }
      else { for (size_t i = 1; i < a.size(); ++i) c.o << (i > 1 ? " " : "") << c.r.expand(a[i]); c.o << "\n"; }
    }
    else if (shell_command(s, *s.out, *s.me, a)) { /* a familiar command handled it */ }
    else if (s.ask_depth > 0) {
      // Already inside a mind's cognitive loop: an unrecognised command is a
      // mistake to report back, not a reason to spawn another ask (which would
      // recurse without bound if the mind keeps typing it).
      fail(Status::NotFound, "unknown command: " + a[0]);
    }
    else {
      // Not a command. Before this line is carried out of the machine to
      // whatever model is bound, THIS machine's own model gets it: it scores
      // every command it was trained on and reports the one it finds least
      // surprising, with the number it chose by.
      //
      // The threshold is 1.0 nats per token and it is not arbitrary: phrasings
      // the model has seen score 0.12-0.39, and one it has not seen scores 4.1.
      // Under it the machine acts; over it, it says it is guessing and the line
      // goes to the bound mind as before. A small model that answered
      // everything would be the same lie as a status surface printing a
      // constant.
      bind_language_model_store(c);
      const auto reply = intelligence::translate_request(raw);
      if (reply.used_model && !reply.command.empty() && reply.command_nll < kLmConfidentNll) {
        c.o << c.o.style(Dim()) << "this machine's own model read that as " << c.o.style(Reset()) << reply.command
            << c.o.style(Dim()) << " (" << reply.command_nll << " nats/token, "
            << intelligence::get_machine_language_model()->model().parameter_count()
            << " parameters trained here)" << c.o.style(Reset()) << "\n";
        c.o << c.o.style(Dim()) << reply.explanation << c.o.style(Reset()) << "\n";
        need(run_line(s, reply.command));
      } else {
        // With no model to hand it to, a line that is not a command is just
        // that: an unknown command. Handing it to the echo reference would
        // print the briefing back and act on its template lines.
        if (!mind_can_answer(s, "mind"))
          fail(Status::NotFound, "unknown command: " + a[0] + " — `help` lists the commands; no model is bound to answer free text (mind set <provider> [model])");
        if (reply.used_model) {
          c.o << c.o.style(Dim()) << "this machine's own model is not confident (" << reply.command_nll
              << " nats/token for \"" << reply.command << "\"), so this goes to the bound mind" << c.o.style(Reset()) << "\n";
        }
        need(ask(s, raw, "mind"));
      }
    }
  } catch (const Fail& f) {
    outcome = f.e;
  }
  if (expect_fail) {
    if (outcome.ok()) { s.failures++; return Result<void>(Status::Conflict, "expected failure but the command succeeded: " + line); }
    c.o << "  refused as expected: " << outcome.error().str() << "\n";
    return Ok();
  }
  if (!outcome.ok()) { s.failures++; return outcome; }
  if (!bind.empty()) s.vars[bind] = s.last;
  return Ok();
}

Result<void> run_script(Session& s, const std::string& text, const std::string& name) {
  size_t n = 0;
  for (auto& line : split(text, '\n')) {
    n++;
    auto trimmed = line.find_first_not_of(" \t\r");
    if (trimmed == std::string::npos || line[trimmed] == '#') continue;
    if (!s.quiet) *s.out << s.out->style(Dim()) << name << ":" << n << " › " << line << s.out->style(Reset()) << "\n";
    auto r = run_line(s, line);
    if (!r.ok()) { *s.out << s.out->style(Red()) << name << ":" << n << " FAILED: " << r.error().str() << s.out->style(Reset()) << "\n"; return r; }
  }
  return Ok();
}

// Exposed for the substrate's own loop: daemons_for and the supervision body
// live in this file's internal namespace, and the desktop's frame loop is the
// only thing on a hosted machine that runs on a clock.
size_t supervise_services(Session& s) { return supervise_services_impl(s); }
size_t start_boot_services(Session& s) { return start_boot_services_impl(s); }

}  // namespace aios::cli
