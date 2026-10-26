// The fleet overlay, implemented. The hub is a blind frame switch that also
// keeps the roster; the client multiplexes byte-circuits over one attach
// connection. Neither understands the node protocol running inside a circuit —
// they move opaque, already-encrypted bytes.
#include "aios/fleet/relay.hpp"
#include "aios/hal/sync.hpp"
#include "aios/node/secure.hpp"
#include "aios/node/node.hpp"
#include "aios/hal/host.hpp"
#include "aios/core/bytes.hpp"
#include "../node/protocol.hpp"
#include <cstring>
#include <cstdio>
#include <atomic>
#include <deque>
#include <unordered_map>
#include <map>
#include "aios/capability/attestation.hpp"
#include "aios/crypto/curve25519.hpp"
#include "aios/core/strbuf.hpp"
#include <memory>

// The link's own state, told to whatever this machine calls a console. It goes
// through the host boundary like everything else the core says, so a bare-metal
// kernel puts it on the serial console and the core touches no stdio.
static void link_note(const std::string& text) { aios::hal::host_note(text); }


namespace aios {

// ---- overlay wire framing (distinct from the node protocol it carries) ------
namespace {
constexpr char RMAGIC[4] = {'A', 'O', 'S', 'R'};
enum RType : uint8_t {
  Challenge = 1, Attach = 2, AttachOk = 3, Roster = 4,
  Open = 5, OpenOk = 6, OpenErr = 7, Incoming = 8, Data = 9, Close = 10,
  Ping = 11, Pong = 12, RErr = 0x7F
};
// Liveness. A member proves it is alive by pinging; the hub evicts a member it
// has not heard from in DEAD_MS and re-announces the roster, so a peer that
// died (or whose process was killed, leaving a half-open socket) does not
// linger and stall a scheduler that tries to reach it.
constexpr uint64_t HEARTBEAT_MS = 6000;
constexpr uint64_t DEAD_MS = 20000;
// A session silent this long has missed two heartbeats: another attach for
// its id may take over (anything fresher keeps the id, see the hub's attach).
constexpr uint64_t STALE_MS = 2 * HEARTBEAT_MS;
constexpr uint64_t REAP_INTERVAL_MS = 2500;

bool wr(hal::Connection& c, hal::Mutex& mu, uint8_t t, ByteSpan p) {
  ByteBuffer f(9 + p.size());
  std::memcpy(f.data(), RMAGIC, 4); f[4] = t;
  uint32_t n = (uint32_t)p.size();
  for (int i = 0; i < 4; ++i) f[5 + i] = uint8_t(n >> (8 * i));
  if (!p.empty()) std::memcpy(f.data() + 9, p.data(), p.size());
  hal::LockGuard g(mu);
  return c.send_all(f);
}
// Reads one frame. The header uses the caller's poll timeout (a timeout with an
// alive peer just means "nothing yet"); once a header is in, the payload gets a
// generous window so a frame split across reads is never mistaken for a hangup.
bool rd(hal::Connection& c, uint8_t& t, ByteBuffer& p, int header_timeout_ms) {
  uint8_t h[9];
  if (!c.recv_all(MutableByteSpan(h, 9), header_timeout_ms)) return false;
  if (std::memcmp(h, RMAGIC, 4) != 0) return false;
  t = h[4];
  uint32_t n = 0; for (int i = 0; i < 4; ++i) n |= uint32_t(h[5 + i]) << (8 * i);
  if (n > (64u << 20)) return false;
  p.resize(n);
  return n == 0 || c.recv_all(p, 15000);
}
bool ct_eq(ByteSpan a, ByteSpan b) {
  if (a.size() != b.size()) return false;
  unsigned d = 0; for (size_t i = 0; i < a.size(); ++i) d |= (unsigned)(a[i] ^ b[i]);
  return d == 0;
}
}  // namespace

// ============================ HUB ===========================================

namespace {
struct MemberCred {
  std::string key;
  ByteBuffer ak, pcrs;
  bool ed_listed = false;   // the line has ed25519=: this member attaches by signature, its key proof is not enough
  ByteBuffer ed25519;       // 32 bytes when well formed
  std::string roles;        // roles=a,b: carried in the signed roster (enforced from stage 4)
};
ByteBuffer from_hex(const std::string& h) {
  ByteBuffer out;
  for (size_t i = 0; i + 1 < h.size(); i += 2) {
    int a = aios::digit_value(h[i], 16), b = aios::digit_value(h[i + 1], 16);
    if (a < 0 || b < 0) return {};
    out.push_back((uint8_t)(a * 16 + b));
  }
  return out;
}
// The members file: `<member-id> <key> [ak=<hex>] [pcrs=<hex>] [ed25519=<hex>] [roles=a,b]` per line, `#` comments. With
// ed25519= the key column may be left out: `<member-id> ed25519=<hex>`.
bool load_members(hal::Host& host, const std::string& path, std::map<std::string, MemberCred>& out) {
  auto b = host.read_file(path);
  if (!b.ok()) return false;
  out.clear();
  for (const std::string& raw : aios::split_lines(std::string(b->begin(), b->end()))) {
    std::vector<std::string> w;
    for (const auto& piece : aios::split_lines(raw, ' ')) if (!piece.empty() && piece != "\r") w.push_back(piece);
    if (w.size() < 2 || w[0][0] == '#') continue;
    MemberCred m;
    size_t opts = 1;
    if (w[1].rfind("ed25519=", 0) != 0) { m.key = w[1]; opts = 2; }
    for (size_t i = opts; i < w.size(); ++i) {
      if (w[i].rfind("ak=", 0) == 0) m.ak = from_hex(w[i].substr(3));
      else if (w[i].rfind("pcrs=", 0) == 0) m.pcrs = from_hex(w[i].substr(5));
      else if (w[i].rfind("ed25519=", 0) == 0) { m.ed_listed = true; m.ed25519 = from_hex(w[i].substr(8)); }
      else if (w[i].rfind("roles=", 0) == 0) m.roles = w[i].substr(6);
    }
    out[w[0]] = std::move(m);
  }
  return true;
}
// A listed id matches the full 32-hex id exactly, or - only when the file uses
// the short form `fleet members` prints - a prefix of it. Nested ids are legal
// (`alpha` and `alphabeta`), so the LONGEST matching key wins; taking the first
// map hit instead would pick a credential by sort order and refuse a listed
// machine with `bad member key`, and a one-character id would match any id
// starting with that character.
const MemberCred* find_member(const std::map<std::string, MemberCred>& m, const std::string& id) {
  const MemberCred* best = nullptr;
  size_t best_len = 0;
  for (const auto& [k, v] : m) {
    if (k.empty() || k.size() > id.size()) continue;
    if (id.compare(0, k.size(), k) != 0) continue;
    if (!best || k.size() > best_len) { best = &v; best_len = k.size(); }
  }
  return best;
}

// The hub's challenge: 16 random bytes, then a flags byte. Bit 0: this hub takes a signed attach. A member's key
// proof (and a TPM quote) covers the whole challenge, so an older member, which proves over whatever it was sent,
// still attaches.
constexpr uint8_t kChallengeSigned = 0x01;
// The roster frame: u32 count and the ids (all an older member reads), then - from a hub with an identity key -
// 'RSTK', a u64 version (the hub's clock, ms), per member its ed25519 key (32 zero bytes: none listed) and roles,
// and last a 64-byte Ed25519 signature by the hub over "aios fleet roster v1\0" and everything before it.
constexpr uint32_t kRosterKeysMagic = 0x4B545352u;   // 'RSTK'
ByteBuffer roster_signed_message(ByteSpan body) {
  static const char kLabel[] = "aios fleet roster v1";
  ByteBuffer m(kLabel, kLabel + sizeof(kLabel));
  m.insert(m.end(), body.begin(), body.end());
  return m;
}
// What a member signs: a fixed label, the hub's challenge and the id it claims.
ByteBuffer attach_transcript(ByteSpan challenge, const Id128& id) {
  static const char kLabel[] = "aios fleet attach v1";
  ByteBuffer t(kLabel, kLabel + sizeof(kLabel));   // with its NUL
  t.insert(t.end(), challenge.begin(), challenge.end());
  Writer w(t); w.id(id);
  return t;
}
}  // namespace

struct RelayHub::Impl {
  std::string key;
  std::string members_path;          // per-member keys; empty: the cluster key admits
  bool require_attest = false;
  uint64_t members_checked_ms = 0;
  std::map<std::string, MemberCred> listed;   // the members file as last read (under mu): keys and roles for the roster
  std::string listed_print;                   // what changed in it, to re-send the roster when a key or role does
  std::function<ByteBuffer(ByteSpan)> signer; // the hub's identity key: signs the roster; empty: ids only
  std::unique_ptr<hal::Listener> listener;
  std::unique_ptr<hal::Host> host;
  hal::Thread acceptor;
  std::atomic<bool> running{false};
  uint16_t port = 0;

  // What goes to a member is queued and written by that member's own writer
  // thread, so nothing in the hub ever blocks on one member's socket - least
  // of all while holding `mu`. (It used to write inline under `mu`: one member
  // that stopped reading - a Pi busy on its one loop - filled its socket, the
  // write blocked with the lock held, and every attach, circuit and even the
  // reaper waited behind it: the whole fleet down for one slow member.) A
  // member that takes more than kMaxQueued or accepts nothing for
  // kStuckSendMs is dropped instead.
  static constexpr size_t kMaxQueued = 16u << 20;
  static constexpr uint64_t kStuckSendMs = 10000;
  struct Member {
    std::shared_ptr<hal::Connection> c;
    Id128 id;
    hal::Mutex send;                 // guards outq
    std::deque<ByteBuffer> outq;
    size_t outq_bytes = 0;
    std::atomic<uint64_t> sending_since{0};   // monotonic ms a write began, 0 when none is in progress
    std::atomic<bool> alive{true};
    std::atomic<uint64_t> last_ms{0}; // monotonic ms of the last frame we heard
    hal::Thread reader;
    hal::Thread writer;
    // Shut the socket down (waking whoever is blocked on it), under the lock
    // that also guards letting it go - so no one touches a released one.
    void close_conn() { hal::LockGuard g(send); if (c) c->close(); }
  };
  hal::Thread reaper;                // evicts members that stopped heart-beating
  mutable hal::Mutex mu;             // guards members + circuits
  std::vector<std::shared_ptr<Member>> members;
  std::unordered_map<uint64_t, std::pair<std::shared_ptr<Member>, std::shared_ptr<Member>>> circuits;
  uint64_t next_circuit = 1;
  uint64_t now_ms() { return host ? host->monotonic_ns() / 1000000ull : 0; }

  std::shared_ptr<Member> find(const Id128& id) {
    for (auto& m : members) if (m->alive && m->id == id) return m;
    return nullptr;
  }
  void push_roster_locked() {
    ByteBuffer body; Writer w(body);
    w.u32((uint32_t)members.size());
    for (auto& m : members) w.id(m->id);
    if (signer) {
      w.u32(kRosterKeysMagic);
      w.u64(host ? host->wall_ns() / 1000000ull : 0);
      for (auto& m : members) {
        const MemberCred* c = find_member(listed, m->id.str());
        uint8_t k[32] = {};
        if (c && c->ed25519.size() == 32) std::memcpy(k, c->ed25519.data(), 32);
        w.raw(ByteSpan(k, 32));
        w.str(c ? c->roles : std::string());
      }
      const ByteBuffer msg = roster_signed_message(ByteSpan(body.data(), body.size()));
      const ByteBuffer sig = signer(ByteSpan(msg.data(), msg.size()));
      if (sig.size() != 64) return;   // never send keys unsigned
      body.insert(body.end(), sig.begin(), sig.end());
    }
    for (auto& m : members) if (m->alive) q(m, Roster, body);
  }
  static std::string print_of(const std::map<std::string, MemberCred>& l) {
    std::string out;
    for (const auto& [id, c] : l) { out += id; out += ' '; out += hex(ByteSpan(c.ed25519.data(), c.ed25519.size())); out += ' '; out += c.roles; out += '\n'; }
    return out;
  }
  // Queue one frame for a member; never blocks. False when the member is gone
  // (or was just dropped for not keeping up).
  bool q(const std::shared_ptr<Member>& m, uint8_t t, ByteSpan p) {
    ByteBuffer f(9 + p.size());
    std::memcpy(f.data(), RMAGIC, 4); f[4] = t;
    uint32_t n = (uint32_t)p.size();
    for (int i = 0; i < 4; ++i) f[5 + i] = uint8_t(n >> (8 * i));
    if (!p.empty()) std::memcpy(f.data() + 9, p.data(), p.size());
    bool overflow = false;
    {
      hal::LockGuard g(m->send);
      if (!m->alive) return false;
      if (m->outq_bytes + f.size() > kMaxQueued) overflow = true;
      else { m->outq_bytes += f.size(); m->outq.push_back(std::move(f)); }
    }
    if (overflow) { m->alive = false; m->close_conn(); return false; }
    return true;
  }
  void start_writer(const std::shared_ptr<Member>& m) {
    Impl* im = this;
    std::shared_ptr<Member> mm = m;
    m->writer = hal::Thread([im, mm] {
      while (im->running && mm->alive) {
        ByteBuffer f;
        {
          hal::LockGuard g(mm->send);
          if (!mm->outq.empty()) { f = std::move(mm->outq.front()); mm->outq.pop_front(); mm->outq_bytes -= f.size(); }
        }
        if (f.empty()) { hal::sleep_ms(2); continue; }
        mm->sending_since = im->now_ms();
        const bool ok = mm->c->send_all(f);
        mm->sending_since = 0;
        if (!ok) { mm->alive = false; mm->close_conn(); }
      }
    });
  }
};


RelayHub::RelayHub(std::string cluster_key) : impl_(new Impl) { impl_->key = std::move(cluster_key); }
void RelayHub::set_members_file(std::string path) { impl_->members_path = std::move(path); }
void RelayHub::set_require_attest(bool on) { impl_->require_attest = on; }
void RelayHub::set_signer(std::function<ByteBuffer(ByteSpan)> sign) { impl_->signer = std::move(sign); }
RelayHub::~RelayHub() { stop(); }
uint16_t RelayHub::port() const { return impl_->port; }
size_t RelayHub::attached() const { hal::LockGuard g(impl_->mu); size_t n = 0; for (auto& m : impl_->members) if (m->alive) ++n; return n; }
std::vector<Id128> RelayHub::members() const { hal::LockGuard g(impl_->mu); std::vector<Id128> v; for (auto& m : impl_->members) if (m->alive) v.push_back(m->id); return v; }

Result<uint16_t> RelayHub::start(uint16_t port, bool loopback_only) {
  auto l = hal::net_listen(port, loopback_only);
  if (!l.ok()) return l.error();
  impl_->listener = std::move(*l);
  impl_->host = hal::make_host();
  impl_->port = impl_->listener->port();
  impl_->running = true;
  Impl* im = impl_.get();
  impl_->acceptor = hal::Thread([im] {
    while (im->running) {
      // A member removed from the members file is dropped within a few seconds.
      if (!im->members_path.empty() && im->now_ms() - im->members_checked_ms > 2000) {
        im->members_checked_ms = im->now_ms();
        std::map<std::string, MemberCred> listed;
        if (load_members(*im->host, im->members_path, listed)) {
          hal::LockGuard g(im->mu);
          for (auto& m : im->members)
            if (m->alive && !find_member(listed, m->id.str())) { m->alive = false; m->close_conn(); }
          // A key or role changed: members get the new roster now, not at the next attach.
          std::string print = Impl::print_of(listed);
          const bool changed = print != im->listed_print;
          im->listed = std::move(listed);
          im->listed_print = std::move(print);
          if (changed) im->push_roster_locked();
        }
      }
      auto c = im->listener->accept(200);
      if (!c.ok()) continue;
      auto member = std::make_shared<Impl::Member>();
      member->c = std::shared_ptr<hal::Connection>(std::move(*c));
      // Challenge/response so only fleet members attach: a key proof (the cluster key, or the member's own key
      // from the members file), or a signature by the identity key the members file lists for the member.
      uint8_t nonce[17]; im->host->random(MutableByteSpan(nonce, 16));
      nonce[16] = im->members_path.empty() ? 0 : kChallengeSigned;
      if (!wr(*member->c, member->send, Challenge, ByteSpan(nonce, sizeof nonce))) continue;
      uint8_t at; ByteBuffer ap;   // the Attach frame
      if (!rd(*member->c, at, ap, 10000) || at != Attach) continue;
      Reader ar(ap); ByteSpan proof_field = ar.bytes(); Id128 id = ar.id();
      if (!ar.ok()) continue;
      // The proof field: the 16-byte key proof, then (from a member that signs) its 64-byte signature.
      if (proof_field.size() != 16 && proof_field.size() != 16 + 64) continue;
      ByteSpan proof(proof_field.data(), 16);
      ByteSpan sig = proof_field.size() == 80 ? ByteSpan(proof_field.data() + 16, 64) : ByteSpan();
      auto refuse = [&](const std::string& why) { wr(*member->c, member->send, RErr, ByteSpan((const uint8_t*)why.data(), why.size())); };
      std::string admit_key = im->key;
      const MemberCred* cred = nullptr;
      std::map<std::string, MemberCred> listed;
      if (!im->members_path.empty()) {
        if (!load_members(*im->host, im->members_path, listed)) { refuse("the hub cannot read its members file"); continue; }
        cred = find_member(listed, id.str());
        if (!cred) { refuse("not a listed member of this fleet"); continue; }
        { hal::LockGuard g(im->mu); im->listed = listed; im->listed_print = Impl::print_of(listed); }
        admit_key = cred->key;
      }
      if (cred && cred->ed_listed) {
        // Listed with an identity key: only a signature by it admits - a copied member key does not.
        if (cred->ed25519.size() != 32) { refuse("the hub's members file lists a malformed ed25519 key for this member"); continue; }
        if (sig.empty()) { refuse("this member is listed with an identity key and must sign its attach (upgrade it)"); continue; }
        crypto::Point32 pub; std::memcpy(pub.data(), cred->ed25519.data(), 32);
        crypto::Sig64 s64; std::memcpy(s64.data(), sig.data(), 64);
        if (!crypto::ed25519_verify(attach_transcript(ByteSpan(nonce, sizeof nonce), id), s64, pub)) { refuse("bad signature: not the identity key listed for this member"); continue; }
      } else {
        auto expect = crypto::derive_key(admit_key, ByteSpan(nonce, sizeof nonce));
        if (!ct_eq(proof, ByteSpan(expect.data(), 16))) { refuse(cred ? "bad member key" : "bad key"); continue; }
      }
      if (im->require_attest) {
        // The quote follows the id: attested, signature, signer, then the PCRs it covers.
        capability::TPMQuote q;
        if (ar.remaining() == 0) { refuse("this hub requires a TPM quote (join with attest=1)"); continue; }
        ByteSpan att = ar.bytes(), sig = ar.bytes(), pub = ar.bytes();
        uint32_t npcr = ar.u32();
        q.attested.assign(att.begin(), att.end()); q.signature.assign(sig.begin(), sig.end()); q.signer_public.assign(pub.begin(), pub.end());
        for (uint32_t i = 0; i < npcr && ar.ok() && i < 24; ++i) {
          capability::PcrValue v; v.index = ar.u32(); ByteSpan d = ar.raw(32);
          if (d.size() == 32) std::memcpy(v.digest.data(), d.data(), 32);
          q.pcrs.push_back(v);
        }
        if (!ar.ok()) { refuse("the quote is malformed"); continue; }
        if (!cred || cred->ak.size() != 65) { refuse("no attestation key is listed for this member"); continue; }
        if (q.signer_public != std::vector<uint8_t>(cred->ak.begin(), cred->ak.end())) { refuse("the quote is signed by a key not listed for this member"); continue; }
        auto v = capability::HardwareAttestor::verify_quote_standalone(q, std::vector<uint8_t>(nonce, nonce + sizeof nonce));
        if (!v.ok() || !*v) { refuse("the quote does not verify: " + (v.ok() ? std::string("bad signature") : v.error().message)); continue; }
        const auto signed_digest = capability::HardwareAttestor::signed_pcr_digest(q);
        if (!cred->pcrs.empty() && (cred->pcrs.size() != 32 || signed_digest != std::vector<uint8_t>(cred->pcrs.begin(), cred->pcrs.end()))) {
          refuse("the quoted PCRs are not the ones listed for this member"); continue;
        }
      }
      member->id = id;
      member->last_ms = im->now_ms();
      // An id is claimed, not proven: any key holder can say any id. So a new
      // attach may take over an id only from a session that is demonstrably
      // gone - silent for STALE_MS (two missed heartbeats). A live session keeps
      // its id and the newcomer is refused; a member reconnecting after a blip
      // is let in on a later retry, once its old session goes quiet or its
      // socket closes (the client retries attach on a backoff).
      auto clash = [&]() -> bool {
        const uint64_t now = im->now_ms();
        for (auto& m : im->members)
          if (m.get() != member.get() && m->alive && m->id == id && !(now > m->last_ms && now - m->last_ms > STALE_MS)) return true;
        return false;
      };
      bool refused;
      { hal::LockGuard g(im->mu); refused = clash(); }
      if (refused) {
        static const char kHeld[] = "id already attached by a live session";
        wr(*member->c, member->send, RErr, ByteSpan((const uint8_t*)kHeld, sizeof(kHeld) - 1));
        continue;
      }
      wr(*member->c, member->send, AttachOk, {});
      {
        hal::LockGuard g(im->mu);
        // Re-checked under the lock that inserts: a live claimant that won the
        // race keeps the id, and this one is closed instead.
        if (clash()) { member->alive = false; member->c->close(); continue; }
        // A member reconnecting (its process survived a hub restart or a blip)
        // supersedes its stale prior session, so the roster never doubles an id.
        for (auto it = im->members.begin(); it != im->members.end();) {
          if ((*it)->alive && (*it)->id == id) { (*it)->alive = false; (*it)->close_conn(); it = im->members.erase(it); }
          else ++it;
        }
        im->members.push_back(member);
        im->push_roster_locked();
      }
      im->start_writer(member);
      std::shared_ptr<Impl::Member> mm = member;
      member->reader = hal::Thread([im, mm] {
        uint8_t t; ByteBuffer p;
        while (im->running && mm->alive) {
          if (!rd(*mm->c, t, p, 250)) {
            if (mm->c->alive()) continue;   // idle
            break;                          // gone
          }
          mm->last_ms = im->now_ms();       // any frame proves the member is alive
          Reader r(p);
          if (t == Ping) {
            im->q(mm, Pong, {});
          } else if (t == Open) {
            uint64_t local = r.u64(); Id128 target = r.id();
            if (!r.ok()) continue;
            hal::LockGuard g(im->mu);
            auto tgt = im->find(target);
            if (!tgt) { ByteBuffer e; Writer w(e); w.u64(local); w.str("no such member"); im->q(mm, OpenErr, e); continue; }
            uint64_t gc = im->next_circuit++;
            im->circuits[gc] = {mm, tgt};
            ByteBuffer ok; Writer w1(ok); w1.u64(local); w1.u64(gc); im->q(mm, OpenOk, ok);
            ByteBuffer in; Writer w2(in); w2.u64(gc); w2.id(mm->id); im->q(tgt, Incoming, in);
          } else if (t == Data) {
            uint64_t gc = r.u64();
            std::shared_ptr<Impl::Member> other;
            { hal::LockGuard g(im->mu); auto it = im->circuits.find(gc); if (it != im->circuits.end()) other = (it->second.first.get() == mm.get()) ? it->second.second : it->second.first; }
            if (other && other->alive) im->q(other, Data, p);   // forward verbatim (gc + bytes)
          } else if (t == Close) {
            uint64_t gc = r.u64();
            std::shared_ptr<Impl::Member> other;
            { hal::LockGuard g(im->mu); auto it = im->circuits.find(gc); if (it != im->circuits.end()) { other = (it->second.first.get() == mm.get()) ? it->second.second : it->second.first; im->circuits.erase(it); } }
            if (other && other->alive) im->q(other, Close, p);
          }
        }
        mm->alive = false;
        // Tear down: drop from roster, close this member's circuits, re-announce.
        std::vector<std::shared_ptr<Impl::Member>> notify;
        { hal::LockGuard g(im->mu);
          for (auto it = im->circuits.begin(); it != im->circuits.end();) {
            if (it->second.first.get() == mm.get() || it->second.second.get() == mm.get()) {
              auto other = (it->second.first.get() == mm.get()) ? it->second.second : it->second.first;
              if (other && other->alive) { ByteBuffer cb; Writer w(cb); w.u64(it->first); im->q(other, Close, cb); }
              it = im->circuits.erase(it);
            } else ++it;
          }
          for (auto it = im->members.begin(); it != im->members.end();) { if (it->get() == mm.get()) it = im->members.erase(it); else ++it; }
          im->push_roster_locked();
        }
        mm->close_conn();
        mm->writer.join();
        // Both of this member's threads are done with its socket: let it go,
        // which frees the descriptor. (The member itself can outlive this -
        // its thread objects hold it - and every departed member used to keep
        // its socket in CLOSE-WAIT forever: 45 on the live hub by evening.)
        std::shared_ptr<hal::Connection> gone;
        { hal::LockGuard g(mm->send); gone.swap(mm->c); }
      });
    }
  });
  // Reaper: evict members that stopped heart-beating. Closing the socket wakes
  // the member's reader, which runs the same teardown path (drop from roster,
  // close its circuits, re-announce) — so a dead peer disappears fleet-wide
  // within DEAD_MS instead of lingering as a half-open session.
  impl_->reaper = hal::Thread([im] {
    while (im->running) {
      hal::sleep_ms((uint32_t)REAP_INTERVAL_MS);
      uint64_t now = im->now_ms();
      std::vector<std::shared_ptr<Impl::Member>> dead;
      { hal::LockGuard g(im->mu);
        for (auto& m : im->members) {
          if (m->alive && m->last_ms && now > m->last_ms && now - m->last_ms > DEAD_MS) dead.push_back(m);
          // A write the member has not taken for kStuckSendMs: it is not reading.
          const uint64_t since = m->sending_since;
          if (m->alive && since && now > since && now - since > Impl::kStuckSendMs) dead.push_back(m);
        } }
      for (auto& m : dead) { m->alive = false; m->close_conn(); }
    }
  });
  return impl_->port;
}

void RelayHub::stop() {
  if (!impl_->running.exchange(false)) return;
  if (impl_->listener) impl_->listener->close();
  if (impl_->acceptor.joinable()) impl_->acceptor.join();
  if (impl_->reaper.joinable()) impl_->reaper.join();
  std::vector<std::shared_ptr<Impl::Member>> ms;
  { hal::LockGuard g(impl_->mu); ms = impl_->members; }
  for (auto& m : ms) { m->alive = false; m->close_conn(); if (m->reader.joinable()) m->reader.join(); }
}

// ============================ CLIENT ========================================

namespace {
struct Circuit {
  hal::Mutex mu;
  std::deque<uint8_t> rx;
  std::atomic<bool> closed{false};
};
}  // namespace

struct RelayClient::Impl {
  std::string key;
  std::string member_key;     // per-member attach key; empty: the cluster key
  bool attest = false;
  std::function<ByteBuffer(ByteSpan)> signer;   // the identity key; empty: key proof only
  Id128 self;
  std::shared_ptr<hal::Connection> c;
  hal::Mutex send;
  std::unique_ptr<hal::Host> host;
  std::string hub_addr, hub_host;
  std::string last_err;   // why the last establish() failed (the thread calling establish)
  std::string err_seen;   // a copy for other threads, under mu
  uint16_t hub_port = 0;
  std::atomic<bool> running{false};
  std::atomic<bool> up{false};
  hal::Thread reader;

  mutable hal::Mutex mu;
  std::vector<Id128> roster;
  // The hub's signed roster keys (set_hub_key): only a roster this key signed, and no older than the last, is used.
  bool have_hub_key = false;
  crypto::Point32 hub_key{};
  uint64_t roster_version = 0;
  bool roster_signed = false;
  std::map<std::string, std::pair<crypto::Point32, std::string>> roster_keys;   // full id -> key (zero: none), roles
  std::string roster_err;
  std::unordered_map<uint64_t, std::shared_ptr<Circuit>> circuits;
  std::deque<std::pair<uint64_t, std::shared_ptr<Circuit>>> incoming;
  struct Pending { std::atomic<bool> done{false}; bool ok = false; uint64_t gc = 0; std::string err; };
  std::unordered_map<uint64_t, std::shared_ptr<Pending>> pending;
  uint64_t next_local = 1;

  // One frame from the hub - shared by the reader thread and poll().
  void handle_frame(uint8_t t, ByteBuffer& p) {
    Reader r(p);
    if (t == Pong) {
      // liveness acknowledged; nothing else to do
    } else if (t == Roster) {
      uint32_t n = r.u32(); std::vector<Id128> v; for (uint32_t i = 0; i < n && r.ok(); ++i) v.push_back(r.id());
      if (r.ok()) { hal::LockGuard g(mu); roster = v; }
      if (r.ok() && have_hub_key) take_roster_keys(p, r, v);
    } else if (t == OpenOk) {
      uint64_t local = r.u64(), gc = r.u64();
      auto cs = std::make_shared<Circuit>();
      hal::LockGuard g(mu);
      circuits[gc] = cs;
      auto it = pending.find(local);
      if (it != pending.end()) { it->second->gc = gc; it->second->ok = true; it->second->done = true; }
    } else if (t == OpenErr) {
      uint64_t local = r.u64(); std::string msg = r.str();
      hal::LockGuard g(mu);
      auto it = pending.find(local);
      if (it != pending.end()) { it->second->err = msg; it->second->ok = false; it->second->done = true; }
    } else if (t == Incoming) {
      uint64_t gc = r.u64(); Id128 from = r.id(); (void)from;
      auto cs = std::make_shared<Circuit>();
      hal::LockGuard g(mu);
      circuits[gc] = cs; incoming.push_back({gc, cs});
    } else if (t == Data) {
      uint64_t gc = r.u64();
      std::shared_ptr<Circuit> cs;
      { hal::LockGuard g(mu); auto it = circuits.find(gc); if (it != circuits.end()) cs = it->second; }
      if (cs) { hal::LockGuard g(cs->mu); cs->rx.insert(cs->rx.end(), p.begin() + 8, p.end()); }
    } else if (t == Close) {
      uint64_t gc = r.u64();
      hal::LockGuard g(mu); auto it = circuits.find(gc); if (it != circuits.end()) it->second->closed = true;
    }
  }

  // Polled mode (poll()): the reader loop, one bounded step at a time, for a
  // kernel that has no spare core for a thread - on the Pi a hal::Thread with
  // no free core runs its body inline, and this body never returns.
  uint64_t poll_last_ping = 0, poll_next_retry = 0;
  uint32_t poll_backoff = 8000;
  uint64_t now_ms() const { return host ? host->monotonic_ns() / 1000000ull : 0; }
  void drop_link() {
    up = false;
    hal::LockGuard g(mu);
    for (auto& kv : circuits) kv.second->closed = true;
    circuits.clear();
    for (auto& kv : pending) { kv.second->err = "hub link reset"; kv.second->ok = false; kv.second->done = true; }
    pending.clear();
    roster.clear();
  }

  bool polled = false;      // attach_polled(): no reader thread - poll_step() is the reader
  bool in_step = false;
  void poll_step() {
    if (in_step) return;
    in_step = true;
    struct Out { bool& f; ~Out() { f = false; } } out{in_step};
    if (!running) return;
    const uint64_t now = now_ms();
    if (up && c) {
      if (now - poll_last_ping >= HEARTBEAT_MS) { send_frame(Ping, {}); poll_last_ping = now; }
      for (int n = 0; n < 32; ++n) {
        const size_t avail = c->pending();
        if (avail == SIZE_MAX || avail < 9) break;          // no complete header yet (or a host that cannot say)
        uint8_t t; ByteBuffer p;
        if (!rd(*c, t, p, 250)) break;
        handle_frame(t, p);
      }
      if (!c->alive()) {
        link_note("fleet: hub link lost - reconnecting\n");
        drop_link();
        poll_backoff = 8000;
        poll_next_retry = now + poll_backoff;
      }
      return;
    }
    if (now < poll_next_retry) return;
    if (establish()) {
      link_note("fleet: re-attached to hub\n");
      poll_last_ping = now_ms();
      poll_backoff = 8000;
    } else {
      poll_backoff = poll_backoff < 60000 ? poll_backoff * 2 : 60000;
      poll_next_retry = now_ms() + poll_backoff;
      link_note("fleet: reconnect failed (" + (last_err.empty() ? std::string("unreachable") : last_err) + "), retry in " + std::to_string(poll_backoff) + " ms\n");
    }
  }

  bool send_frame(uint8_t t, ByteSpan p) { return c && wr(*c, send, t, p); }

  // The keys part of a roster, when the pinned hub key signed it. An unsigned or badly signed roster leaves the keys
  // this member already trusts as they are (dropping them would loosen every check that uses them).
  void take_roster_keys(const ByteBuffer& p, Reader& r, const std::vector<Id128>& ids) {
    auto fail = [&](const std::string& why) {
      hal::LockGuard g(mu);
      if (roster_err != why && host) host->log_error("fleet: roster keys not taken: " + why);
      roster_err = why;
    };
    if (p.size() < 64 || r.remaining() < 4 + 8 + 64 || r.u32() != kRosterKeysMagic) return fail("the hub does not sign its roster");
    const uint64_t version = r.u64();
    std::map<std::string, std::pair<crypto::Point32, std::string>> keys;
    for (const auto& id : ids) {
      ByteSpan k = r.raw(32);
      std::string roles = r.str();
      if (!r.ok() || k.size() != 32) return fail("malformed roster keys");
      crypto::Point32 pk{}; std::memcpy(pk.data(), k.data(), 32);
      keys[id.str()] = {pk, roles};
    }
    if (r.remaining() != 64) return fail("malformed roster keys");
    const ByteBuffer msg = roster_signed_message(ByteSpan(p.data(), p.size() - 64));
    crypto::Sig64 sig{}; std::memcpy(sig.data(), p.data() + p.size() - 64, 64);
    if (!crypto::ed25519_verify(ByteSpan(msg.data(), msg.size()), sig, hub_key)) return fail("the roster is not signed by the pinned hub key");
    hal::LockGuard g(mu);
    if (version < roster_version) { roster_err = "an older roster was replayed"; return; }
    roster_version = version;
    roster_keys = std::move(keys);
    roster_signed = true;
    roster_err.clear();
  }

  // Connect to the hub and complete the challenge/response attach on a fresh
  // socket, then swap it in as the live connection. Used for the first attach
  // and for every reconnect — so a hub restart or a network blip heals itself
  // instead of leaving the member silently detached.
  bool establish() {
    auto cc = hal::net_connect(hub_host, hub_port, 8000);
    if (!cc.ok()) { last_err = cc.error().message; return false; }
    auto conn = std::shared_ptr<hal::Connection>(std::move(*cc));
    uint8_t t; ByteBuffer p;
    if (!rd(*conn, t, p, 8000) || t != Challenge) { last_err = "no challenge from the hub"; return false; }
    auto proof = crypto::derive_key(member_key.empty() ? key : member_key, p);
    ByteBuffer proof_field(proof.data(), proof.data() + 16);
    if (signer && p.size() == 17 && (p[16] & kChallengeSigned)) {
      ByteBuffer s = signer(attach_transcript(p, self));
      if (s.size() == 64) proof_field.insert(proof_field.end(), s.begin(), s.end());
    }
    ByteBuffer ab; Writer w(ab); w.bytes(proof_field); w.id(self);
    if (attest) {
      // Scoped: the TPM is let go before the hub is waited on.
      capability::HardwareAttestor tpm;
      if (!tpm.attach().ok() || !tpm.available()) { last_err = "attest=1, but this machine has no TPM to quote with"; return false; }
      auto q = tpm.generate_tpm2_quote({0, 1, 2, 3, 7}, std::vector<uint8_t>(p.begin(), p.end()));
      if (!q.ok()) { last_err = "the TPM would not quote: " + q.error().message; return false; }
      w.bytes(ByteSpan(q->attested.data(), q->attested.size()));
      w.bytes(ByteSpan(q->signature.data(), q->signature.size()));
      w.bytes(ByteSpan(q->signer_public.data(), q->signer_public.size()));
      w.u32((uint32_t)q->pcrs.size());
      for (const auto& v : q->pcrs) { w.u32(v.index); w.raw(ByteSpan(v.digest.data(), 32)); }
    }
    if (!wr(*conn, send, Attach, ab)) { last_err = "attach send failed"; return false; }
    if (!rd(*conn, t, p, 8000)) { last_err = "the hub closed the link (wrong cluster key?)"; return false; }
    if (t == RErr) { last_err = "the hub refused: " + std::string(p.begin(), p.end()); return false; }
    if (t != AttachOk) { last_err = "unexpected answer from the hub"; return false; }
    last_err.clear();
    { hal::LockGuard g(send); c = conn; }   // publish the live connection under the send lock
    up = true;
    return true;
  }

  CommandRunner command_runner;
  hal::Thread command_server;
  std::atomic<bool> command_server_running{false};
  void ensure_command_server(std::shared_ptr<Impl> self_sp);
};

// A circuit presented as a byte stream. Everything above — the secure
// handshake, the node protocol, migration — runs on this without knowing the
// hub exists.
namespace {
class RelayedConnection final : public hal::Connection {
 public:
  RelayedConnection(std::shared_ptr<RelayClient::Impl> impl, uint64_t gc, std::shared_ptr<Circuit> cs)
    : impl_(std::move(impl)), gc_(gc), cs_(std::move(cs)) {}
  ~RelayedConnection() override { close(); }
  bool send_all(ByteSpan data) override {
    if (cs_->closed) return false;
    ByteBuffer body(8 + data.size());
    for (int i = 0; i < 8; ++i) body[i] = uint8_t(gc_ >> (8 * i));
    if (!data.empty()) std::memcpy(body.data() + 8, data.data(), data.size());
    return impl_->send_frame(Data, body);
  }
  bool recv_all(MutableByteSpan dst, int timeout_ms) override {
    size_t got = 0;
    // A REAL clock, not an iteration count times an assumed 2ms: polled
    // mode's poll_step() drains up to 32 frames per call, each with its own
    // 250ms rd() timeout - one call can genuinely cost up to 8s. Counting
    // "waited += 2" per loop pass ignored that entirely, so a caller's
    // 5000ms telemetry() timeout could actually take up to 2500 * 8s in the
    // worst case - indistinguishable from a dead hang on real hardware,
    // requiring a physical power cycle. Confirmed on the Pi: fleet_survey()
    // logged "telemetry() start" and nothing after it.
    const uint64_t start = impl_->now_ms();
    while (got < dst.size()) {
      { hal::LockGuard g(cs_->mu);
        while (got < dst.size() && !cs_->rx.empty()) { dst[got++] = cs_->rx.front(); cs_->rx.pop_front(); } }
      if (got == dst.size()) return true;
      if (cs_->closed) return false;
      if (timeout_ms >= 0 && impl_->now_ms() - start >= (uint64_t)timeout_ms) return false;
      // Polled client: nobody else is reading the hub link - read it here,
      // and let the network run, or this circuit's bytes never arrive.
      if (impl_->polled) { impl_->poll_step(); hal::net_service(2); }
      else hal::sleep_ms(2);
    }
    return true;
  }
  std::string peer() const override { return "relay:" + std::to_string(gc_); }
  void close() override {
    if (cs_->closed.exchange(true)) return;
    ByteBuffer body(8); for (int i = 0; i < 8; ++i) body[i] = uint8_t(gc_ >> (8 * i));
    impl_->send_frame(Close, body);
    hal::LockGuard g(impl_->mu); impl_->circuits.erase(gc_);
  }
  bool alive() const override { return !cs_->closed && impl_->up; }
  size_t pending() const override { hal::LockGuard g(cs_->mu); return cs_->rx.size(); }
 private:
  std::shared_ptr<RelayClient::Impl> impl_;
  uint64_t gc_;
  std::shared_ptr<Circuit> cs_;
};
}  // namespace

// Impl is shared so RelayedConnections keep it alive as long as they exist.
struct RelayClientHolder { std::shared_ptr<RelayClient::Impl> impl; };

// The public class holds only an opaque holder; reach the shared Impl through it.
#define IM (static_cast<RelayClientHolder*>(holder_)->impl)

void RelayClient::set_member_key(std::string key) { IM->member_key = std::move(key); }
void RelayClient::set_attest(bool on) { IM->attest = on; }
void RelayClient::set_signer(std::function<ByteBuffer(ByteSpan)> sign) { IM->signer = std::move(sign); }
void RelayClient::set_hub_key(const std::array<uint8_t, 32>& key) { IM->hub_key = key; IM->have_hub_key = true; }
bool RelayClient::peer_key(const Id128& peer, std::array<uint8_t, 32>& key) const {
  hal::LockGuard g(IM->mu);
  auto it = IM->roster_keys.find(peer.str());
  if (it == IM->roster_keys.end()) return false;
  static const crypto::Point32 zero{};
  if (it->second.first == zero) return false;
  key = it->second.first;
  return true;
}
std::string RelayClient::peer_roles(const Id128& peer) const {
  hal::LockGuard g(IM->mu);
  auto it = IM->roster_keys.find(peer.str());
  return it == IM->roster_keys.end() ? std::string() : it->second.second;
}
bool RelayClient::roster_signed() const { hal::LockGuard g(IM->mu); return IM->roster_signed; }

RelayClient::RelayClient(std::string cluster_key, Id128 self) {
  auto im = std::make_shared<Impl>();
  im->key = std::move(cluster_key); im->self = self; im->host = hal::make_host();
  holder_ = new RelayClientHolder{im};
}
RelayClient::~RelayClient() { stop(); delete static_cast<RelayClientHolder*>(holder_); }

bool RelayClient::attached() const { return IM->up; }
bool RelayClient::trying() const { return IM->running && !IM->up; }
std::string RelayClient::last_error() const { hal::LockGuard g(IM->mu); return IM->err_seen; }
const std::string& RelayClient::hub_address() const { return IM->hub_addr; }
std::vector<Id128> RelayClient::roster() const {
  hal::LockGuard g(IM->mu); std::vector<Id128> v;
  for (auto& id : IM->roster) if (!(id == IM->self)) v.push_back(id);
  return v;
}

Result<void> RelayClient::attach(const std::string& hub_host, uint16_t hub_port, bool keep_trying) {
  auto im = IM;
  im->hub_host = hub_host; im->hub_port = hub_port;
  im->hub_addr = hub_host + ":" + std::to_string(hub_port);
  if (!im->establish()) {
    const std::string why = im->last_err.empty() ? std::string("unreachable") : im->last_err;
    if (!keep_trying) return Result<void>(Status::IoError, "could not attach to hub " + im->hub_addr + ": " + why);
    // A member configured to be in this fleet (AIOS_FLEET_HUB) whose hub is
    // not up yet keeps trying, on the same backoff as a reconnect.
    { hal::LockGuard g(im->mu); im->err_seen = why; }
    link_note("fleet: hub " + im->hub_addr + " unreachable (" + why + ") - retrying in the background\n");
  }
  im->running = true;
  Impl* raw = im.get();
  im->reader = hal::Thread([raw] {
    auto now_ms = [raw] { return raw->host ? raw->host->monotonic_ns() / 1000000ull : 0; };
    uint32_t backoff = 500;
    while (raw->running && !raw->up) {   // the first attach, when the hub was down
      hal::sleep_ms(backoff);
      if (raw->establish()) { { hal::LockGuard g(raw->mu); raw->err_seen.clear(); } link_note("fleet: attached to hub " + raw->hub_addr + "\n"); backoff = 500; break; }
      { hal::LockGuard g(raw->mu); raw->err_seen = raw->last_err; }
      backoff = backoff < 8000 ? backoff * 2 : 8000;
    }
    uint64_t last_ping = now_ms();
    while (raw->running) {
      // --- read frames until the link drops ---
      uint8_t t; ByteBuffer p;
      while (raw->running) {
        // Heartbeat: prove we are alive so the hub does not reap us. Runs on
        // every idle poll too, so an otherwise-silent member still beats.
        uint64_t now = now_ms();
        if (now - last_ping >= HEARTBEAT_MS) { raw->send_frame(Ping, {}); last_ping = now; }
        if (!rd(*raw->c, t, p, 250)) { if (raw->c->alive()) continue; break; }
        raw->handle_frame(t, p);
      }
      if (!raw->running) break;
      // --- link dropped: fail everything riding the old connection, then
      //     reconnect with backoff so a hub restart heals without a bounce. ---
      link_note("fleet: hub link lost - reconnecting\n");
      raw->up = false;
      { hal::LockGuard g(raw->mu);
        for (auto& kv : raw->circuits) kv.second->closed = true;
        raw->circuits.clear();
        for (auto& kv : raw->pending) { kv.second->err = "hub link reset"; kv.second->ok = false; kv.second->done = true; }
        raw->pending.clear();
        raw->roster.clear();
      }
      while (raw->running) {
        hal::sleep_ms(backoff);
        if (raw->establish()) { { hal::LockGuard g(raw->mu); raw->err_seen.clear(); } link_note("fleet: re-attached to hub\n"); backoff = 500; last_ping = now_ms(); break; }
        { hal::LockGuard g(raw->mu); raw->err_seen = raw->last_err; }
        link_note("fleet: reconnect failed (" + (raw->last_err.empty() ? std::string("unreachable") : raw->last_err) + "), retry in " + std::to_string(backoff) + " ms\n");
        backoff = backoff < 8000 ? backoff * 2 : 8000;
      }
    }
    raw->up = false;
  });
  return Ok();
}

Result<void> RelayClient::attach_polled(const std::string& hub_host, uint16_t hub_port) {
  auto im = IM;
  im->hub_host = hub_host; im->hub_port = hub_port;
  im->hub_addr = hub_host + ":" + std::to_string(hub_port);
  if (!im->establish())
    return Result<void>(Status::IoError, "could not attach to hub " + im->hub_addr + ": " + (im->last_err.empty() ? std::string("unreachable") : im->last_err));
  im->running = true;
  im->polled = true;
  im->poll_last_ping = im->now_ms();
  return Ok();
}

// Everything the reader thread does, without waiting: heartbeat when due,
// handle whatever complete frames have arrived, notice a dropped link, and
// reconnect on a backoff (the one step that can block - a connect - happens
// at most once per backoff interval).
void RelayClient::poll() { IM->poll_step(); }

Result<std::unique_ptr<hal::Connection>> RelayClient::open(const Id128& target) {
  auto im = IM;
  if (!im->up) return Result<std::unique_ptr<hal::Connection>>(Status::Busy, "not attached to a hub");
  uint64_t local; std::shared_ptr<Impl::Pending> pend = std::make_shared<Impl::Pending>();
  { hal::LockGuard g(im->mu); local = im->next_local++; im->pending[local] = pend; }
  ByteBuffer ob; Writer w(ob); w.u64(local); w.id(target);
  if (!im->send_frame(Open, ob)) return Result<std::unique_ptr<hal::Connection>>(Status::IoError, "open send failed");
  // A REAL clock, not an iteration count times an assumed 2ms - the same fix
  // as RelayedConnection::recv_all() needed: polled mode's poll_step() can
  // cost up to 8s in a single call (draining up to 32 frames, each with its
  // own 250ms rd() timeout), so counting iterations here let this "8s" wait
  // balloon to minutes in practice.
  const uint64_t open_start = im->now_ms();
  // A polled client has no reader: the hub's OpenOk only arrives if this loop
  // reads the link itself (the Pi opening a circuit waited out all 8 s here).
  while (!pend->done && im->now_ms() - open_start < 8000) {
    if (im->polled) { im->poll_step(); hal::net_service(2); }
    else hal::sleep_ms(2);
  }
  { hal::LockGuard g(im->mu); im->pending.erase(local); }
  if (!pend->done) return Result<std::unique_ptr<hal::Connection>>(Status::Expired, "hub did not answer open");
  if (!pend->ok) return Result<std::unique_ptr<hal::Connection>>(Status::NotFound, pend->err);
  std::shared_ptr<Circuit> cs;
  { hal::LockGuard g(im->mu); auto it = im->circuits.find(pend->gc); if (it != im->circuits.end()) cs = it->second; }
  if (!cs) return Result<std::unique_ptr<hal::Connection>>(Status::Corrupt, "circuit vanished");
  return std::unique_ptr<hal::Connection>(new RelayedConnection(im, pend->gc, cs));
}

std::unique_ptr<hal::Connection> RelayClient::accept_incoming() {
  auto im = IM;
  uint64_t gc; std::shared_ptr<Circuit> cs;
  { hal::LockGuard g(im->mu); if (im->incoming.empty()) return nullptr; auto f = im->incoming.front(); im->incoming.pop_front(); gc = f.first; cs = f.second; }
  return std::unique_ptr<hal::Connection>(new RelayedConnection(im, gc, cs));
}

void RelayClient::Impl::ensure_command_server(std::shared_ptr<Impl> self_sp) {
  if (command_server_running.exchange(true)) return;
  command_server = hal::Thread([self_sp] {
    while (self_sp->running && self_sp->command_server_running) {
      std::unique_ptr<hal::Connection> conn;
      {
        hal::LockGuard g(self_sp->mu);
        if (!self_sp->incoming.empty()) {
          auto f = self_sp->incoming.front();
          self_sp->incoming.pop_front();
          conn.reset(new RelayedConnection(self_sp, f.first, f.second));
        }
      }
      if (!conn) {
        hal::sleep_ms(20);
        continue;
      }
      auto shared_conn = std::shared_ptr<hal::Connection>(std::move(conn));
      hal::Thread([self_sp, shared_conn] {
        auto plain = make_plain_link(shared_conn);
        Id128 peer_kernel;
        ByteBuffer peer_payload;
        // No devices here (a command server), the device list's kernel marker, then this side's feature word: the form
        // a node's hello payload has, so a caller reads it - it once sent nothing, which callers read as corrupt.
        ByteBuffer mine;
        {
          Writer w(mine);
          w.u32(0);
          w.u32(0x4C4E524Bu);
          const ByteBuffer feat = link_feature_payload();
          w.raw(ByteSpan(feat.data(), feat.size()));
        }
        auto skey = secure_handshake_responder(*plain, self_sp->key, *self_sp->host, self_sp->self, peer_kernel, peer_payload, ByteSpan(mine.data(), mine.size()));
        if (!skey.ok()) return;
        auto link = make_secure_link(shared_conn, *skey, false);
        std::string roles;   // the signed roster's roles for this peer; empty: not limited
        {
          bool upgraded = false; crypto::Key k2{};
          const auto lid = link_identity();
          auto x = link_identity_exchange(*link, *skey, false, self_sp->self, peer_kernel, ByteSpan(peer_payload.data(), peer_payload.size()), *self_sp->host, lid.get(), upgraded, k2);
          if (!x.ok()) { self_sp->host->log_error("fleet: circuit from " + peer_kernel.str() + " refused: " + x.error().message); return; }
          if (upgraded) link = make_secure_link(shared_conn, k2, false);
          std::array<uint8_t, 32> pk{};
          if (upgraded && lid && lid->peer_roles && lid->peer_key && lid->peer_key(peer_kernel, pk)) roles = lid->peer_roles(peer_kernel);
        }
        link->set_compress(link_peer_deflates(ByteSpan(peer_payload.data(), peer_payload.size())));
        uint8_t t;
        ByteBuffer body;
        while (self_sp->running && self_sp->command_server_running && link->recv(t, body, 120000)) {
          if (t == nodeproto::Command) {
            Reader r(body);
            std::string line = r.str();
            std::string out;
            const char* role = command_role(line);
            if (!roles_allow(roles, role)) {
              ByteBuffer rep;
              Writer w(rep);
              w.u8(static_cast<uint8_t>(Status::Denied));
              w.str("peer " + peer_kernel.str() + " has no " + role + " role here (its roles: " + roles + ")");
              if (!link->send(nodeproto::Error, rep)) break;
              continue;
            }
            int code = self_sp->command_runner ? self_sp->command_runner(line, out) : fleet_shell_command(line, out);
            ByteBuffer rep;
            Writer w(rep);
            w.u32(static_cast<uint32_t>(code));
            w.str(out);
            if (!link->send(nodeproto::Command, rep)) break;
          } else {
            ByteBuffer rep;
            Writer w(rep);
            w.u8(static_cast<uint8_t>(Status::Unsupported));
            w.str("unsupported operation on relay client");
            link->send(nodeproto::Error, rep);
          }
        }
      }).detach();
    }
  });
}

void RelayClient::set_command_runner(CommandRunner r) {
  auto im = IM;
  im->command_runner = std::move(r);
  im->ensure_command_server(im);
}

Result<std::pair<int, std::string>> RelayClient::command(const Id128& target, const std::string& line, int timeout_ms) {
  auto im = IM;
  auto conn = open(target);
  if (!conn.ok()) return conn.error();
  auto node = std::make_shared<RemoteNode>(std::shared_ptr<hal::Connection>(std::move(*conn)), "relay:" + target.short_str());
  auto h = node->hello(im->key, im->self, *im->host);
  if (!h.ok()) return h.error();
  return node->command(line);
}

Result<ByteBuffer> RelayClient::query(const Id128& target, const std::string& sql, int timeout_ms) {
  auto im = IM;
  auto conn = open(target);
  if (!conn.ok()) return conn.error();
  auto node = std::make_shared<RemoteNode>(std::shared_ptr<hal::Connection>(std::move(*conn)), "relay:" + target.short_str());
  auto h = node->hello(im->key, im->self, *im->host);
  if (!h.ok()) return h.error();
  return node->query(sql, timeout_ms);
}

void RelayClient::stop() {
  auto im = IM;
  if (!im->running.exchange(false)) return;
  im->command_server_running = false;
  im->up = false;
  if (im->c) im->c->close();
  if (im->reader.joinable()) im->reader.join();
  if (im->command_server.joinable()) im->command_server.join();
}

#undef IM

}  // namespace aios
