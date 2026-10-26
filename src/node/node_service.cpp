// The serving side: a remote kernel presents a token and becomes a guest
// entity here. Every request runs through that guest's Surface, so the same
// capability rules govern a peer kernel that govern a local entity.
#include "aios/node/node.hpp"
#include "aios/hal/sync.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "protocol.hpp"

namespace aios {

static bool same_secret(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned d = 0; for (size_t i = 0; i < a.size(); ++i) d |= (unsigned)(a[i] ^ b[i]);
  return d == 0;
}
static void reply_error(Link& l, Status st, const std::string& msg) { ByteBuffer b; Writer w(b); w.u8((uint8_t)st); w.str(msg); l.send(nodeproto::Error, b); }

NodeService::NodeService(Kernel& k, std::string token) : k_(k) { tokens_.push_back(std::move(token)); }

EntityID NodeService::admission() {
  hal::LockGuard g(mu_);
  if (!admission_.is_null() && k_.entities().exists(admission_)) return admission_;
  auto a = k_.spawn("peers:admission");
  if (!a.ok()) return EntityID{};
  admission_ = *a;
  // The admission set, grantable so a migrated entity's authority can be
  // re-issued from it: compute, scheduling, memory within the spawn quota,
  // its own state and channels. Nothing over the kernel or storage.
  k_.grant(admission_, ResourceRef{ResourceKind::ComputeDevice, {}}, R_Execute | R_Grant);
  k_.grant(admission_, ResourceRef{ResourceKind::Kernel, {}}, R_Schedule | R_Checkpoint | R_Grant);
  k_.grant(admission_, ResourceRef{ResourceKind::Memory, {}}, R_Allocate | R_Read | R_Write | R_Grant, 0, k_.config().default_memory_quota);
  k_.grant(admission_, ResourceRef{ResourceKind::State, {}}, R_Read | R_Write | R_Allocate | R_Destroy | R_Grant);
  k_.grant(admission_, ResourceRef{ResourceKind::Channel, {}}, R_Allocate | R_Publish | R_Consume | R_Grant);
  return admission_;
}

void NodeService::add_token(const std::string& token) {
  hal::LockGuard g(mu_);
  for (const auto& t : tokens_) if (same_secret(t, token)) return;
  tokens_.push_back(token);
}
NodeService::~NodeService() { stop(); }

Result<uint16_t> NodeService::start(uint16_t port, bool loopback_only) {
  auto l = hal::net_listen(port, loopback_only);
  if (!l.ok()) return l.error();
  listener_ = std::move(*l);
  port_ = listener_->port();
  running_ = true;
  acceptor_ = hal::Thread([this] { accept_loop(); });
  return port_;
}

void NodeService::stop() {
  if (!running_.exchange(false)) return;
  if (listener_) listener_->close();
  if (acceptor_.joinable()) acceptor_.join();
  // Close every session's socket before joining: a session sits in step()'s
  // hour-long recv timeout, checked only between reads, so `running_` going
  // false does nothing to a thread already inside that wait — closing the fd
  // out from under it is what actually returns it early.
  for (auto& s : sessions_) {
    if (s.conn) s.conn->close();
    if (s.t.joinable()) s.t.join();
  }
}

std::vector<PeerInfo> NodeService::peers() const { hal::LockGuard g(mu_); return peers_; }

// Small adapter so the session owns a shared connection without copying it around.
struct Shim final : hal::Connection {
  std::shared_ptr<hal::Connection> c;
  explicit Shim(std::shared_ptr<hal::Connection> x) : c(std::move(x)) {}
  bool send_all(ByteSpan d) override { return c->send_all(d); }
  bool recv_all(MutableByteSpan d, int t) override { return c->recv_all(d, t); }
  std::string peer() const override { return c->peer(); }
  void close() override { c->close(); }
};

void NodeService::accept_loop() {
  while (running_) {
    auto c = listener_->accept(200);
    if (!c.ok()) continue;
    hal::LockGuard g(mu_);
    start_session(std::shared_ptr<hal::Connection>(std::move(*c)));
  }
}

void NodeService::start_session(std::shared_ptr<hal::Connection> conn) {
  // Reap the ones that have finished: join (they have returned) and drop
  // their connections.
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    if (it->done && it->done->load()) { if (it->t.joinable()) it->t.join(); it = sessions_.erase(it); }
    else ++it;
  }
  auto done = std::make_shared<std::atomic<bool>>(false);
  Threaded s;
  s.conn = conn;
  s.done = done;
  s.t = hal::Thread([this, conn, done]() mutable {
    serve(std::unique_ptr<hal::Connection>(new Shim(conn)));
    done->store(true);
  });
  sessions_.push_back(std::move(s));
}

struct NodeService::Session {
  std::shared_ptr<hal::Connection> c;
  std::unique_ptr<Link> link;
  EntityID guest;
  size_t slot = 0;
  bool alive = true;
};

std::shared_ptr<NodeService::Session> NodeService::admit(std::shared_ptr<hal::Connection> c) {
  auto plain = make_plain_link(c);
  Id128 peer_kernel; ByteBuffer peer_payload;
  ByteBuffer devices;
  {
    Writer w(devices);
    auto devs = k_.devices().all();
    w.u32((uint32_t)devs.size());
    for (auto* d : devs) { w.id(d->id()); w.u8((uint8_t)d->type()); w.str(d->name()); w.u32(d->capacity().lanes); w.u64(d->capacity().memory); }
    // Each device's named kernels, after the list an older peer reads (it
    // stops there): 'KRNL', then per device a count and the hashes.
    w.u32(0x4C4E524Bu);
    for (auto* d : devs) {
      const auto ks = d->custom_kernels();
      w.u32((uint32_t)ks.size());
      for (uint64_t k : ks) w.u64(k);
    }
    w.u32(nodeproto::kFeatureMagic);
    w.u32(nodeproto::kFeatExecBatch | nodeproto::kFeatDeflate);
  }
  std::vector<std::string> tokens;
  { hal::LockGuard g(mu_); tokens = tokens_; }
  auto key = secure_handshake_responder_any(*plain, tokens, k_.host(), k_.self(), peer_kernel, peer_payload, devices);
  if (!key.ok()) { reply_error(*plain, Status::Denied, "node token rejected"); return nullptr; }
  auto s = std::make_shared<Session>();
  s->c = c;
  s->link = make_secure_link(c, *key, false);
  s->link->set_compress(link_peer_deflates(ByteSpan(peer_payload.data(), peer_payload.size())));
  auto guest = k_.spawn("peer:" + c->peer());
  if (!guest.ok()) { reply_error(*s->link, guest.status(), guest.error().message); return nullptr; }
  s->guest = *guest;
  // A peer may compute and schedule here; allocation stays within the spawn quota.
  // R_Grant on the admission set lets a migrated entity be re-derived from it.
  k_.grant(*guest, ResourceRef{ResourceKind::ComputeDevice, {}}, R_Execute | R_Grant);
  k_.grant(*guest, ResourceRef{ResourceKind::Kernel, {}}, R_Schedule | R_Grant);
  k_.grant(*guest, ResourceRef{ResourceKind::Memory, {}}, R_Allocate | R_Read | R_Write | R_Grant, 0, k_.config().default_memory_quota);
  k_.grant(*guest, ResourceRef{ResourceKind::State, {}}, R_Read | R_Write | R_Allocate | R_Destroy | R_Grant);
  k_.grant(*guest, ResourceRef{ResourceKind::Channel, {}}, R_Allocate | R_Publish | R_Consume | R_Grant);
  { hal::LockGuard g(mu_); peers_.push_back(PeerInfo{*guest, c->peer(), peer_kernel}); s->slot = peers_.size() - 1; }
  return s;
}

// A session is over: its guest entity goes with it - and with the guest,
// everything it held here (memory a peer staged, graphs, capabilities).
// Until this, every connection a peer ever made left its guest behind.
void NodeService::end_session(Session& sess) {
  {
    hal::LockGuard g(mu_);
    peers_[sess.slot].connected = false;
  }
  if (!sess.guest.is_null() && k_.entities().exists(sess.guest)) (void)k_.terminate(sess.guest);
  sess.guest = EntityID{};
}

void NodeService::serve(std::unique_ptr<hal::Connection> conn) {
  auto s = admit(std::shared_ptr<hal::Connection>(std::move(conn)));
  if (!s) return;
  while (running_ && step(*s, 1000 * 60 * 60)) {}
  end_session(*s);
}

void NodeService::enable() { running_ = true; }

// Serve one connection we did not accept ourselves — a relay circuit a peer
// opened to us through the hub. It runs the same responder handshake and
// request loop as a direct peer, so a relayed guest is a guest like any other.
void NodeService::serve_connection(std::shared_ptr<hal::Connection> c) {
  running_ = true;
  hal::LockGuard g(mu_);
  start_session(std::move(c));
}

bool NodeService::adopt_polled(std::shared_ptr<hal::Connection> c) {
  running_ = true;
  polled_mode_ = true;
  auto s = admit(std::move(c));
  if (!s) return false;
  polled_.push_back(s);
  return true;
}

Result<uint16_t> NodeService::start_polled(uint16_t port, bool loopback_only) {
  auto l = hal::net_listen(port, loopback_only);
  if (!l.ok()) return l.error();
  listener_ = std::move(*l);
  port_ = listener_->port();
  running_ = true; polled_mode_ = true;
  return port_;
}

void NodeService::poll() {
  if (!running_ || !polled_mode_) return;
  if (listener_) {
    auto c = listener_->accept(0);
    if (c.ok()) { auto s = admit(std::shared_ptr<hal::Connection>(std::move(*c))); if (s) polled_.push_back(s); }
  }
  for (auto it = polled_.begin(); it != polled_.end();) {
    if (!(*it)->alive || !step(**it, 0)) { if (!(*it)->alive) { end_session(**it); it = polled_.erase(it); continue; } }
    ++it;
  }
}

// One request from a peer. Returns false when the link is gone (a timeout with
// nothing pending returns true in polled mode so the caller keeps going).
bool NodeService::step(Session& sess, int timeout_ms) {
  Link& link = *sess.link;
  // Escalation mitigation: verify guest identity exists and is assigned to this session
  if (sess.guest.is_null() || !k_.entities().exists(sess.guest)) {
    sess.alive = false;
    return false;
  }
  Surface s = k_.as(sess.guest);
  auto guest = Result<EntityID>(sess.guest);
  uint8_t t; ByteBuffer p;
  if (!link.recv(t, p, timeout_ms)) {
    if (timeout_ms == 0 && sess.c->alive()) return true;   // nothing pending yet
    sess.alive = false; return false;
  }
  { hal::LockGuard g(mu_); peers_[sess.slot].requests++; }
    Reader r(p);
    ByteBuffer out; Writer w(out);
    Result<void> st = Ok();
    switch (t) {
      case nodeproto::MemAlloc: {
        uint64_t size = r.u64();
        auto m = s.allocate(size, MemoryType::System);
        if (!m.ok()) { st = m.error(); break; }
        // Zeroed, on every substrate: a caller skips sending the zero bytes of
        // what it stages here, so they must already be zero.
        (void)k_.memory().fill(*m, 0);
        w.id(*m);
        break;
      }
      case nodeproto::MemWrite: { MemoryID id{r.id()}; uint64_t off = r.u64(); ByteSpan data = r.bytes(); st = s.write(id, off, data); break; }
      case nodeproto::MemRead: {
        MemoryID id{r.id()}; uint64_t off = r.u64(), len = r.u64();
        // A caller that appends 1 takes the data without its trailing zero
        // bytes and zero-fills the rest itself: a 32 MB output buffer holding a
        // 200 KB answer crosses as 200 KB. (An older caller appends nothing.)
        const bool trim = r.remaining() >= 1 && r.u8() == 1;
        auto d = s.read(id, off, len);
        if (!d.ok()) { st = d.error(); break; }
        if (trim) { size_t n = d->size(); while (n > 0 && (*d)[n - 1] == 0) --n; w.bytes(ByteSpan(d->data(), n)); }
        else w.bytes(*d);
        break;
      }
      case nodeproto::MemRelease: { MemoryID id{r.id()}; st = s.release(id); break; }
      case nodeproto::Exec: {
        auto op = Operation::decode(r);
        if (!op.ok()) { st = op.error(); break; }
        auto g = s.create_graph();
        if (!g.ok()) { st = g.error(); break; }
        auto a = s.add_operation(*g, *op);
        if (!a.ok()) { st = a.error(); k_.graphs().destroy(*g); break; }
        auto rep = s.execute(*g);
        k_.graphs().destroy(*g);
        if (!rep.ok()) { st = rep.error(); break; }
        if (!rep->failure.empty()) { st = Result<void>(Status::IoError, rep->failure); break; }
        w.u64(rep->elapsed_ns); w.str(rep->placements.empty() ? "" : rep->placements[0].device_name);
        break;
      }
      case nodeproto::ExecBatch: {
        const uint32_t nw = r.u32();
        for (uint32_t i = 0; i < nw && st.ok() && r.ok(); ++i) {
          MemoryID id{r.id()}; const uint64_t off = r.u64(); ByteSpan d = r.bytes();
          st = s.write(id, off, d);
        }
        if (!st.ok()) break;
        auto op = Operation::decode(r);
        if (!op.ok()) { st = op.error(); break; }
        auto g = s.create_graph();
        if (!g.ok()) { st = g.error(); break; }
        auto a = s.add_operation(*g, *op);
        if (!a.ok()) { st = a.error(); k_.graphs().destroy(*g); break; }
        auto rep = s.execute(*g);
        k_.graphs().destroy(*g);
        if (!rep.ok()) { st = rep.error(); break; }
        if (!rep->failure.empty()) { st = Result<void>(Status::IoError, rep->failure); break; }
        w.u64(rep->elapsed_ns); w.str(rep->placements.empty() ? "" : rep->placements[0].device_name);
        const uint32_t nr = r.u32();
        w.u32(nr);
        for (uint32_t i = 0; i < nr && st.ok(); ++i) {
          MemoryID id{r.id()}; const uint64_t off = r.u64(), len = r.u64(); const bool trim = r.u8() == 1;
          auto d = s.read(id, off, len);
          if (!d.ok()) { st = d.error(); break; }
          size_t n = d->size();
          if (trim) while (n > 0 && (*d)[n - 1] == 0) --n;
          w.bytes(ByteSpan(d->data(), n));
        }
        break;
      }
      case nodeproto::ChanCreate: { uint32_t cap = r.u32(); uint64_t slotb = r.u64(); auto ch = s.create_channel(cap, slotb); if (!ch.ok()) st = ch.error(); else w.id(*ch); break; }
      case nodeproto::ChanPublish: {
        ChannelID ch{r.id()};
        StateObject so; so.id = StateID{r.id()}; so.kind = (StateKind)r.u8(); so.generation = r.u64();
        EntityID publisher{r.id()};
        ByteSpan payload = r.bytes(); so.payload.assign(payload.begin(), payload.end());
        uint32_t nm = r.u32(); for (uint32_t i = 0; i < nm && r.ok(); ++i) so.memory.push_back(MemoryID{r.id()});
        so.owner = publisher;
        auto need = k_.capabilities().require(sess.guest, ResourceRef{ResourceKind::Channel, ch}, R_Publish, k_.now());
        if (!need.ok()) { st = need; break; }
        auto seq = k_.channels().publish(ch, publisher, so, k_.now());
        if (!seq.ok()) st = seq.error(); else w.u64(*seq);
        break;
      }
      case nodeproto::Migrate: {
        // A peer in our token-authenticated domain hands us a running entity's
        // image: identity, state, memory, graphs and execution position come
        // back verbatim; its capabilities do not. Each is re-derived from the
        // admission entity's authority (which outlives this session), and
        // dropped where it holds no such delegable authority, so an image
        // cannot carry more than this machine admits (the same rule as a
        // console restore).
        ByteSpan image = r.bytes();
        const EntityID admit = admission();
        if (admit.is_null()) { st = Result<void>(Status::IoError, "no admission entity"); break; }
        auto id = k_.restore_image(image, admit);
        if (!id.ok()) st = id.error(); else w.id(*id);
        break;
      }
      case nodeproto::Telemetry: {
        // A peer asks how loaded we are. Reporting load is not a capability-
        // gated action — it reveals only what the machine would tell any peer
        // in its trusted domain — so it answers straight from the kernel.
        k_.telemetry().encode(w);
        break;
      }
      case nodeproto::Replicate: {
        // A peer hands us a recovery point for one of ITS jobs: an image we
        // store but do not run, so the job can reappear here if that peer dies.
        EntityID ent{r.id()};
        uint64_t gen = r.u64();
        ByteSpan image = r.bytes();
        Id128 src; { hal::LockGuard g(mu_); src = peers_[sess.slot].kernel; }
        st = k_.store_replica(src, ent, gen, image);
        break;
      }
      case nodeproto::ChanConsume: {
        ChannelID ch{r.id()};
        auto f = s.consume(ch);
        if (!f.ok()) { st = f.error(); break; }
        w.u64(f->sequence); w.id(f->publisher); w.id(f->state); w.u8((uint8_t)f->kind); w.u64(f->generation); w.u64(f->published_ns); w.bytes(f->payload);
        w.u32((uint32_t)f->memory.size()); for (auto& m : f->memory) w.id(m);
        break;
      }
      case nodeproto::MemPatch: {
        MemoryID id{r.id()};
        uint64_t off = r.u64();
        ByteSpan data = r.bytes();
        st = s.write(id, off, data);
        break;
      }
      case nodeproto::StateDelta: {
        StateID id{r.id()};
        StateKind kind = (StateKind)r.u8();
        uint64_t target_gen = r.u64();
        ByteSpan payload = r.bytes();
        uint32_t nm = r.u32();
        std::vector<MemoryID> mems;
        for (uint32_t i = 0; i < nm && r.ok(); ++i) mems.push_back(MemoryID{r.id()});
        auto res = s.apply_state_delta(id, kind, target_gen, payload, mems);
        if (!res.ok()) st = res.error();
        else w.u64(*res);
        break;
      }
      case nodeproto::StateSync: {
        StateID id{r.id()};
        uint64_t client_gen = r.u64();
        auto so = s.state(id);
        if (!so.ok()) { st = so.error(); break; }
        if (so->generation == client_gen && client_gen != 0) {
          w.u8(1); // 1 = Up-to-date / 0-payload transfer
          w.u64(so->generation);
        } else {
          w.u8(0); // 0 = Full state updated
          so->encode(w);
        }
        break;
      }
      case nodeproto::Command: {
        std::string line = r.str();
        if (!r.ok()) break;
        std::string output;
        int code = runner_ ? runner_(line, output) : execute_host_command(line, output);
        w.u32(static_cast<uint32_t>(code)); w.str(output);
        break;
      }
      case nodeproto::Sql: {
        std::string sql = r.str();
        if (!r.ok()) break;
        if (!sql_runner_) { st = Result<void>(Status::Unsupported, "this node does not serve SQL"); break; }
        auto res = sql_runner_(sql);
        if (!res.ok()) { st = res.error(); break; }
        out = std::move(*res);
        break;
      }
      default: st = Result<void>(Status::Invalid, "unknown node request");
    }
    if (!r.ok() && st.ok()) st = Result<void>(Status::Corrupt, "malformed node request");
    if (!st.ok()) reply_error(link, st.status(), st.error().message);
    else if (!link.send(t, out)) { sess.alive = false; return false; }
  return true;
}

}  // namespace aios
