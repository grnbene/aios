#include <algorithm>
// The attaching side: a remote kernel becomes devices, a memory provider and a
// channel transport in this kernel. Memory objects of type Remote are mirrored
// locally and synchronised by generation, exactly like accelerator memory.
#include "aios/node/node.hpp"
#include "aios/hal/sync.hpp"
#include "aios/hal/process.hpp"
#include "aios/kernel/kernel.hpp"
#include "protocol.hpp"
#include <unordered_map>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include "aios/hal/sync.hpp"

namespace aios {

// A region held on a peer: its id there, and the link to the peer holding it
// (a region always goes back to the peer that made it, whichever is chosen
// for new regions since).
struct RemoteHandle { MemoryID remote; std::shared_ptr<RemoteNode> node; };

RemoteNode::RemoteNode(std::shared_ptr<hal::Connection> c, std::string address) : c_(std::move(c)), address_(std::move(address)) {}

Result<ByteBuffer> RemoteNode::call(uint8_t type, ByteSpan payload, int timeout_ms) {
  hal::LockGuard g(mu_);
  calls_++;
  if (!link_) return Result<ByteBuffer>(Status::Invalid, "node link not established");
  if (!link_->send(type, payload)) return Result<ByteBuffer>(Status::IoError, "node " + address_ + " closed the link");
  uint8_t t; ByteBuffer reply;
  if (!link_->recv(t, reply, timeout_ms)) return Result<ByteBuffer>(Status::IoError, "no reply from node " + address_ + " (or the link failed authentication)");
  if (t == nodeproto::Error) { Reader r(reply); Status st = (Status)r.u8(); std::string msg = r.str(); return Result<ByteBuffer>(st, "node " + address_ + ": " + msg); }
  if (t != type) return Result<ByteBuffer>(Status::Corrupt, "node replied with the wrong frame");
  return reply;
}

void RemoteNode::close() {
  hal::LockGuard g(mu_);
  if (c_) c_->close();
}

Result<std::pair<int, std::string>> RemoteNode::command(const std::string& line) {
  ByteBuffer body; Writer w(body);
  w.str(line);
  auto rep = call(nodeproto::Command, body);
  if (!rep.ok()) return rep.error();
  Reader r(*rep);
  int code = static_cast<int>(r.u32());
  std::string out = r.str();
  if (!r.ok()) return Result<std::pair<int, std::string>>(Status::Corrupt, "node " + address_ + " sent a malformed command reply");
  return std::make_pair(code, std::move(out));
}

Result<ByteBuffer> RemoteNode::query(const std::string& sql, int timeout_ms) {
  ByteBuffer body; Writer w(body);
  w.str(sql);
  return call(nodeproto::Sql, body, timeout_ms);
}

int fleet_shell_command(const std::string& line, std::string& output) {
  static const bool allowed = hal::process_host().env("AIOS_FLEET_SHELL") == "1";
  if (!allowed) {
    output = "this machine runs no shell commands for the fleet (its AIOS_FLEET_SHELL=1 would allow them)\n";
    return 126;
  }
  return execute_host_command(line, output);
}

int execute_host_command(const std::string& line, std::string& output) {
  output.clear();
  // Through the host's process facility (the core does not popen itself): the shell runs the line, its output and
  // errors come back together, as `2>&1` gave them. A day's ceiling stands in for popen's none.
#if defined(_WIN32)
  std::vector<std::string> argv = {"cmd", "/c", line};
#else
  std::vector<std::string> argv = {"/bin/sh", "-c", line};
#endif
  auto r = hal::run_tool(argv, 24u * 3600u * 1000u);
  if (!r.ok()) {
    output = "failed to execute command\n";
    return 127;
  }
  output = std::move(r->output);
  return r->exit_code;
}

Result<NodeTelemetry> RemoteNode::telemetry() {
  // A quick status ping, not a long-running operation - 5s, not the
  // 120s default. A survey over several roster members (fleet_survey(),
  // desktop.cpp's Fleet tab) calls this once per member SYNCHRONOUSLY on
  // the single frame-loop core; one stale/unresponsive entry at 120s each
  // is what actually happened on real hardware - the board's own fleet
  // link (and, since fleet_poll()'s reentrancy guard gates BOTH the
  // outbound hub connection and NodeService::poll() together, its ability
  // to answer fleet run/sql_on from anyone) went unserviced for minutes,
  // looking indistinguishable from a full hang until the timeout(s)
  // finally elapsed on their own.
  auto rep = call(nodeproto::Telemetry, {}, 5000);
  if (!rep.ok()) return rep.error();
  Reader r(*rep);
  NodeTelemetry t = NodeTelemetry::decode(r);
  if (!r.ok()) return Result<NodeTelemetry>(Status::Corrupt, "node " + address_ + " sent malformed telemetry");
  return t;
}

Result<void> RemoteNode::replicate(const EntityID& entity, uint64_t generation, ByteSpan image) {
  ByteBuffer body; Writer w(body);
  w.id(entity); w.u64(generation); w.bytes(image);
  auto rep = call(nodeproto::Replicate, body);
  if (!rep.ok()) return rep.error();
  return Ok();
}

Result<void> RemoteNode::mem_patch(const MemoryID& remote, uint64_t offset, ByteSpan delta) {
  ByteBuffer body; Writer w(body);
  w.id(remote); w.u64(offset); w.bytes(delta);
  auto rep = call(nodeproto::MemPatch, body);
  if (!rep.ok()) return rep.error();
  return Ok();
}

Result<uint64_t> RemoteNode::state_delta(const StateID& state, StateKind kind, uint64_t target_gen, ByteSpan delta_payload, const std::vector<MemoryID>& memory_refs) {
  ByteBuffer body; Writer w(body);
  w.id(state); w.u8((uint8_t)kind); w.u64(target_gen);
  w.bytes(delta_payload);
  w.u32((uint32_t)memory_refs.size());
  for (const auto& m : memory_refs) w.id(m);
  auto rep = call(nodeproto::StateDelta, body);
  if (!rep.ok()) return rep.error();
  Reader r(*rep);
  return r.u64();
}

Result<std::pair<bool, StateObject>> RemoteNode::state_sync(const StateID& state, uint64_t client_gen) {
  ByteBuffer body; Writer w(body);
  w.id(state); w.u64(client_gen);
  auto rep = call(nodeproto::StateSync, body);
  if (!rep.ok()) return rep.error();
  Reader r(*rep);
  uint8_t flag = r.u8();
  if (flag == 1) {
    StateObject so;
    so.id = state;
    so.generation = r.u64();
    return std::make_pair(false, so); // false = not modified / 0 bytes payload
  }
  auto so = StateObject::decode(r);
  if (!so.ok()) return so.error();
  return std::make_pair(true, *so); // true = modified
}

Result<void> RemoteNode::hello(const std::string& token, const Id128& my_kernel, hal::Host& host) {
  // Authenticated handshake on the plain link, then everything is encrypted.
  auto plain = make_plain_link(c_);
  ByteBuffer peer_payload;
  const ByteBuffer mine = link_feature_payload();   // what this side reads (deflated frames), for the peer to use
  auto key = secure_handshake_initiator(*plain, token, host, my_kernel, kernel_, peer_payload, ByteSpan(mine.data(), mine.size()));
  if (!key.ok()) return key.error();
  link_ = make_secure_link(c_, *key, true);
  {
    // Both sides newer: rekey from fresh X25519 keys, each side signing with its identity key.
    bool upgraded = false; crypto::Key k2{};
    const auto lid = link_identity();
    auto x = link_identity_exchange(*link_, *key, true, my_kernel, kernel_, ByteSpan(peer_payload.data(), peer_payload.size()), host, lid.get(), upgraded, k2);
    if (!x.ok()) { link_.reset(); return x.error(); }
    if (upgraded) link_ = make_secure_link(c_, k2, true);
  }
  link_->set_compress(link_peer_deflates(ByteSpan(peer_payload.data(), peer_payload.size())));
  // A member that only answers commands (an app host's relay command server, RelayClient::ensure_command_server)
  // completes the handshake with nothing to say: no devices. Read as a device count, that was "corrupt: hello reply",
  // and `fleet run` could not reach the very members that install a command runner.
  if (peer_payload.empty()) return Ok();
  Reader rd(peer_payload);
  uint32_t n = rd.u32();
  for (uint32_t i = 0; i < n && rd.ok(); ++i) { RemoteDeviceInfo d; d.id = DeviceID{rd.id()}; d.type = (DeviceType)rd.u8(); d.name = rd.str(); d.lanes = rd.u32(); d.memory = rd.u64(); devices_.push_back(d); }
  if (!rd.ok()) return Result<void>(Status::Corrupt, "hello reply");
  // A newer peer says which named kernels each device runs.
  if (rd.remaining() >= 4 && rd.u32() == 0x4C4E524Bu) {
    for (auto& d : devices_) {
      const uint32_t k = rd.u32();
      for (uint32_t j = 0; j < k && rd.ok(); ++j) d.kernels.push_back(rd.u64());
    }
    if (!rd.ok()) return Result<void>(Status::Corrupt, "hello reply (kernels)");
    if (rd.remaining() >= 8 && rd.u32() == nodeproto::kFeatureMagic) features_ = rd.u32();
  }
  return Ok();
}

namespace {

class RemoteMemoryProvider final : public MemoryProvider {
 public:
  RemoteMemoryProvider(hal::Host& host, std::shared_ptr<RemoteNode> node) : host_(host) { add(std::move(node)); }
  MemoryType type() const override { return MemoryType::Remote; }
  uint64_t capacity() const override {
    auto n = chosen(); if (!n) return 0;
    uint64_t m = 0; for (auto& d : n->devices()) m = std::max(m, d.memory); return m;
  }
  Result<hal::Mapping> allocate(uint64_t size, const MemoryID&) override {
    auto node = chosen();
    if (!node) return Result<hal::Mapping>(Status::Unsupported, "no attached peer provides remote memory");
    auto m = host_.map(size, hal::MappingKind::Private);
    if (!m.ok()) return m;
    ByteBuffer b; Writer w(b); w.u64(size);
    auto r = node->call(nodeproto::MemAlloc, b);
    if (!r.ok()) { host_.unmap(*m); return r.error(); }
    Reader rd(*r);
    m->device_ctx = new RemoteHandle{MemoryID{rd.id()}, node};
    return m;
  }
  void release(hal::Mapping& m) override {
    if (m.device_ctx) { auto* h = static_cast<RemoteHandle*>(m.device_ctx); ByteBuffer b; Writer w(b); w.id(h->remote); if (h->node) h->node->call(nodeproto::MemRelease, b); delete h; m.device_ctx = nullptr; }
    host_.unmap(m);
  }
  // A peer attached later is a candidate too; a re-reached peer replaces the
  // dead link it had (same address) and keeps its place as the chosen one.
  void add(std::shared_ptr<RemoteNode> node) {
    hal::LockGuard g(mu_);
    for (auto& n : nodes_) if (n->address() == node->address()) { if (chosen_ == n) chosen_ = node; n = node; return; }
    nodes_.push_back(node);
    if (!chosen_) chosen_ = node;
  }
  void choose(std::shared_ptr<RemoteNode> node) { add(node); hal::LockGuard g(mu_); chosen_ = node; }
  std::shared_ptr<RemoteNode> chosen() const { hal::LockGuard g(mu_); return chosen_; }
 private:
  hal::Host& host_;
  mutable hal::Mutex mu_;
  std::vector<std::shared_ptr<RemoteNode>> nodes_;
  std::shared_ptr<RemoteNode> chosen_;
};

class RemoteDevice final : public Device {
 public:
  RemoteDevice(const DeviceID& id, std::shared_ptr<RemoteNode> node, RemoteDeviceInfo info) : id_(id), node_(std::move(node)), info_(std::move(info)) {
    name_ = "remote:" + node_->address() + "/" + info_.name;
    switch (info_.type) { case DeviceType::GPU: case DeviceType::RemoteGPU: type_ = DeviceType::RemoteGPU; break; case DeviceType::NPU: case DeviceType::RemoteNPU: type_ = DeviceType::RemoteNPU; break; default: type_ = DeviceType::RemoteCPU; }
    cap_.lanes = info_.lanes; cap_.memory = info_.memory; cap_.native_memory = MemoryType::Remote;
    cap_.flops_per_second = uint64_t(info_.lanes) * 2ull * 1000000000ull;
  }
  const DeviceID& id() const override { return id_; }
  DeviceType type() const override { return type_; }
  const std::string& name() const override { return name_; }
  const DeviceCapacity& capacity() const override { return cap_; }
  uint32_t busy_lanes() const override { return busy_.load(); }
  // A named kernel crosses when the peer's device said it runs it; the op is
  // encoded with its kernel hash and the peer's device runs its own copy.
  bool supports(const Operation& op) const override {
    if (op.code != OpCode::Custom) return true;
    return std::find(info_.kernels.begin(), info_.kernels.end(), op.custom_kernel) != info_.kernels.end();
  }
  std::vector<uint64_t> custom_kernels() const override { return info_.kernels; }
  Result<void> register_kernel(const std::string&, CustomKernel) override { return Result<void>(Status::Unsupported, "a kernel is registered on the machine that runs it, not through its remote device"); }

  struct Staged { MemoryID local, remote; bool temp; uint64_t size; uint8_t* host; bool output; };
  // `via` is the link the op runs on: the shared one, or a job's own sibling
  // (whose session holds none of the shared link's remote memory, so every
  // operand is staged fresh on it).
  Result<Staged> stage(MemoryFabric& fabric, const Tensor& t, bool output, RemoteNode& via, bool sibling) {
    const MemoryObject* o = fabric.lookup(t.memory);
    if (!o) return Result<Staged>(Status::NotFound, "memory " + t.memory.short_str());
    Staged s{t.memory, {}, false, o->size, static_cast<uint8_t*>(o->base()), output};
    // A remote region is used in place only on the peer that holds it; one
    // held by another peer is staged fresh like any other operand.
    if (!sibling && o->type == MemoryType::Remote && o->mapping.device_ctx &&
        static_cast<RemoteHandle*>(o->mapping.device_ctx)->node.get() == node_.get()) {
      s.remote = static_cast<RemoteHandle*>(o->mapping.device_ctx)->remote;
      hal::LockGuard g(mu_);
      auto it = uploaded_.find(t.memory);
      if (it == uploaded_.end() || it->second != o->generation) {
        ByteBuffer b; Writer w(b); w.id(s.remote); w.u64(0); w.bytes(ByteSpan(s.host, o->size));
        auto r = via.call(nodeproto::MemWrite, b); if (!r.ok()) return r.error();
        uploaded_[t.memory] = o->generation;
      }
    } else {
      s.temp = true;
      ByteBuffer b; Writer w(b); w.u64(o->size);
      auto r = via.call(nodeproto::MemAlloc, b); if (!r.ok()) return r.error();
      Reader rd(*r); s.remote = MemoryID{rd.id()};
      // A fresh allocation on the peer is zeroed, so trailing zeros need not
      // travel - and an all-zero buffer (an output waiting to be written, a
      // proof buffer) does not travel at all. Pushing a 32 MB empty buffer
      // through the hub stalled the Pi's loop for 44 s.
      uint64_t n = o->size;
      while (n > 0 && s.host[n - 1] == 0) --n;
      if (n > 0) {
        ByteBuffer wb; Writer ww(wb); ww.id(s.remote); ww.u64(0); ww.bytes(ByteSpan(s.host, n));
        auto wr = via.call(nodeproto::MemWrite, wb); if (!wr.ok()) return wr.error();
      }
    }
    return s;
  }

  Result<void> execute(const Operation& op, MemoryFabric& fabric, uint32_t lanes) override {
    struct Busy { std::atomic<uint32_t>& b; uint32_t n; Busy(std::atomic<uint32_t>& x, uint32_t k) : b(x), n(k) { b += n; } ~Busy() { b -= n; } } busy(busy_, std::max<uint32_t>(1, lanes));
    // A named kernel is a whole job: it runs on this device's job lane - a
    // link of its own - so the shared link (SQL, telemetry, mirrors) is never
    // held for its duration.
    if (op.code == OpCode::Custom && node_->can_open_sibling()) {
      auto r = execute_job(op, fabric);
      // The peer restarted: its devices have new ids, and the one this device stands for is gone from its fabric
      // ("pinned device ... is not in the fabric"). A named kernel runs on whichever of the peer's devices has it,
      // so the pin goes and the job is asked once more - instead of every job failing until this side restarts.
      if (!r.ok() && !unpinned_.load() && r.error().message.find("is not in the fabric") != std::string::npos) {
        unpinned_.store(true);
        r = execute_job(op, fabric);
      }
      return r;
    }
    RemoteNode& via = *node_;
    std::vector<Staged> st;
    auto release_temps = [&]() { for (auto& s : st) if (s.temp) { ByteBuffer b; Writer w(b); w.id(s.remote); via.call(nodeproto::MemRelease, b); } };
    Operation rop = op;
    for (size_t i = 0; i < op.inputs.size(); ++i) { auto s = stage(fabric, op.inputs[i], false, via, false); if (!s.ok()) { release_temps(); return s.error(); } st.push_back(*s); rop.inputs[i].memory = s->remote; }
    size_t nin = st.size();
    for (size_t i = 0; i < op.outputs.size(); ++i) {
      bool shared = false;
      for (size_t k = 0; k < nin; ++k) if (st[k].local == op.outputs[i].memory) { Staged c = st[k]; c.output = true; c.temp = false; st.push_back(c); st[k].output = false; rop.outputs[i].memory = c.remote; shared = true; break; }
      if (!shared) { auto s = stage(fabric, op.outputs[i], true, via, false); if (!s.ok()) { release_temps(); return s.error(); } st.push_back(*s); rop.outputs[i].memory = s->remote; }
    }
    pin(rop);
    ByteBuffer b; Writer w(b); rop.encode(w);
    auto r = via.call(nodeproto::Exec, b, 120000);
    if (!r.ok()) { release_temps(); return r.error(); }
    // Outputs come home into the local mirrors.
    for (auto& s : st) {
      if (!s.output) continue;
      auto rd = read_back(via, s.remote, s.size, s.host);
      if (!rd.ok()) { release_temps(); return rd; }
      fabric.touch(s.local);
      if (!s.temp) { hal::LockGuard g(mu_); const MemoryObject* o = fabric.lookup(s.local); if (o) uploaded_[s.local] = o->generation; }
    }
    // temps that were shared in/out were marked temp=false on the output copy; release by the input record
    release_temps();
    return Ok();
  }
  // The op as the peer runs it: pinned to the peer's own device.
  void pin(Operation& rop) const {
    rop.requirement.affinity = unpinned_.load() ? DeviceID{} : info_.id;
    rop.requirement.device = DeviceType::Any;
    // The selector that chose THIS device here ('remote', 'remote:host/...')
    // names nothing in the peer's fabric, and its scheduler consults the
    // selector before the affinity. The pin is the whole instruction.
    rop.requirement.device_selector.clear();
    rop.depends.clear();
  }
  static Result<void> read_back(RemoteNode& via, const MemoryID& remote, uint64_t size, uint8_t* host) {
    ByteBuffer rb; Writer rw(rb); rw.id(remote); rw.u64(0); rw.u64(size); rw.u8(1);   // 1: trailing zeros may be left off
    auto rd = via.call(nodeproto::MemRead, rb);
    if (!rd.ok()) return rd.error();
    Reader rr(*rd); ByteSpan data = rr.bytes();
    const size_t got = std::min<size_t>(data.size(), size);
    std::memcpy(host, data.data(), got);
    if (got < size) std::memset(host + got, 0, size - got);
    return Ok();
  }

  // ---- the job lane --------------------------------------------------------
  // One link of this device's own, kept open between jobs, and the operands
  // staged on it, kept too: a fleet worker sending cube after cube re-sends
  // only what changed (the cube), not the formula, the empty output buffers
  // and a fresh handshake every time. Each staged buffer mirrors a local one;
  // `generation` is the local generation the remote copy matches.
  struct JobStage { MemoryID remote; uint64_t size = 0; uint64_t generation = ~0ull; uint64_t last_use = 0; };
  static constexpr size_t kJobStageMax = 32;
  std::atomic<bool> unpinned_{false};   // the peer no longer has info_.id (it restarted): named kernels go unpinned

  // A device with N cores may take N jobs at once: a pool of lanes, each its
  // own session on the peer (so each keeps its own staged operands), up to
  // the device's lanes. A job prefers the lane that already holds its first
  // operand - a fleet worker keeps landing where its formula already is.
  struct Lane {
    hal::Mutex mu;
    std::shared_ptr<RemoteNode> job_;
    std::unordered_map<MemoryID, JobStage, TypedIdHash<2>> job_stage_;
    uint64_t job_clock_ = 0;
  };
  Lane& pick_lane(const Operation& op) {
    hal::LockGuard g(lanes_mu_);
    const MemoryID want = op.inputs.empty() ? MemoryID{} : op.inputs[0].memory;
    // A lane's staging map is only looked at while nobody uses the lane.
    for (auto& l : lanes_) if (l->users == 0 && l->stage_has(want)) { ++l->users; return l->lane; }
    for (auto& l : lanes_) if (l->users == 0) { ++l->users; return l->lane; }
    const size_t cap = std::max<uint32_t>(1, info_.lanes);
    if (lanes_.size() < cap) { lanes_.push_back(std::make_unique<Slot>()); ++lanes_.back()->users; return lanes_.back()->lane; }
    // every lane busy and the pool full: queue on the least used
    Slot* best = lanes_.front().get();
    for (auto& l : lanes_) if (l->users < best->users) best = l.get();
    ++best->users;
    return best->lane;
  }
  void release_lane(Lane& lane) {
    hal::LockGuard g(lanes_mu_);
    for (auto& l : lanes_) if (&l->lane == &lane && l->users > 0) --l->users;
  }
  struct Slot {
    Lane lane;
    uint32_t users = 0;   // jobs holding or queued on this lane
    bool stage_has(const MemoryID& m) const { return !m.is_null() && lane.job_stage_.count(m) > 0; }
  };
  hal::Mutex lanes_mu_;
  std::vector<std::unique_ptr<Slot>> lanes_;

  Result<void> execute_job(const Operation& op, MemoryFabric& fabric) {
    Lane& lane = pick_lane(op);
    struct Release { RemoteDevice* d; Lane& l; ~Release() { d->release_lane(l); } } release{this, lane};
    hal::LockGuard g(lane.mu);
    auto& job_ = lane.job_;
    auto& job_stage_ = lane.job_stage_;
    auto& job_clock_ = lane.job_clock_;
    auto drop_lane = [&]() { if (job_) job_->close(); job_.reset(); job_stage_.clear(); };
    if (job_ && !job_->alive()) drop_lane();
    if (!job_) {
      auto j = node_->open_sibling();
      if (!j.ok()) return Result<void>(j.error().status, "a job lane for " + name_ + ": " + j.error().message);
      job_ = *j;
    }
    RemoteNode& via = *job_;
    const uint64_t use = ++job_clock_;
    // A staged copy whose local buffer has been released is never asked for again (a new buffer has a new id): it
    // is freed on the peer now. Kept until evicted, an app route's fresh 64 MB answer buffer a request held 32 of
    // them - 2 GB - on the peer, and its app host was killed for it.
    for (auto k = job_stage_.begin(); k != job_stage_.end();) {
      if (fabric.lookup(k->first)) { ++k; continue; }
      ByteBuffer b; Writer w(b); w.id(k->second.remote); (void)via.call(nodeproto::MemRelease, b);
      k = job_stage_.erase(k);
    }
    // One round trip for the whole job where the peer speaks ExecBatch: the
    // uploads and the reads ride with the operation.
    const bool batch = (via.features() & nodeproto::kFeatExecBatch) != 0;
    std::vector<std::pair<MemoryID, ByteBuffer>> writes;
    auto fail = [&](const Error& e) -> Result<void> { drop_lane(); return Result<void>(e.status, e.message); };
    auto stage_one = [&](const Tensor& t, bool send) -> Result<MemoryID> {
      const MemoryObject* o = fabric.lookup(t.memory);
      if (!o) return Result<MemoryID>(Status::NotFound, "memory " + t.memory.short_str());
      auto it = job_stage_.find(t.memory);
      if (it != job_stage_.end() && it->second.size != o->size) {
        ByteBuffer b; Writer w(b); w.id(it->second.remote); (void)via.call(nodeproto::MemRelease, b);
        job_stage_.erase(it);
        it = job_stage_.end();
      }
      bool fresh = false;
      if (it == job_stage_.end()) {
        // Room first: the least recently used buffer not in this op goes.
        while (job_stage_.size() >= kJobStageMax) {
          auto victim = job_stage_.end();
          for (auto k = job_stage_.begin(); k != job_stage_.end(); ++k)
            if (k->second.last_use != use && (victim == job_stage_.end() || k->second.last_use < victim->second.last_use)) victim = k;
          if (victim == job_stage_.end()) break;
          ByteBuffer b; Writer w(b); w.id(victim->second.remote); (void)via.call(nodeproto::MemRelease, b);
          job_stage_.erase(victim);
        }
        ByteBuffer b; Writer w(b); w.u64(o->size);
        auto r = via.call(nodeproto::MemAlloc, b);
        if (!r.ok()) return r.error();
        Reader rd(*r);
        JobStage js; js.remote = MemoryID{rd.id()}; js.size = o->size;
        it = job_stage_.emplace(t.memory, js).first;
        fresh = true;   // zeroed on the peer
      }
      JobStage& js = it->second;
      js.last_use = use;
      if (js.generation != o->generation) {
        const uint8_t* host = static_cast<const uint8_t*>(o->base());
        uint64_t n = o->size;
        if (fresh) while (n > 0 && host[n - 1] == 0) --n;   // the peer's copy is zero past here already
        if (!send && fresh) n = 0;                          // an output nobody has written yet: zero is right
        if (n > 0 && batch) {
          writes.emplace_back(js.remote, ByteBuffer(host, host + n));
        } else if (n > 0) {
          ByteBuffer wb; Writer ww(wb); ww.id(js.remote); ww.u64(0); ww.bytes(ByteSpan(host, n));
          auto wr = via.call(nodeproto::MemWrite, wb);
          if (!wr.ok()) return wr.error();
        }
        js.generation = o->generation;
      }
      return js.remote;
    };
    Operation rop = op;
    for (size_t i = 0; i < op.inputs.size(); ++i) {
      auto m = stage_one(op.inputs[i], true);
      if (!m.ok()) return fail(m.error());
      rop.inputs[i].memory = *m;
    }
    for (size_t i = 0; i < op.outputs.size(); ++i) {
      auto m = stage_one(op.outputs[i], false);
      if (!m.ok()) return fail(m.error());
      rop.outputs[i].memory = *m;
    }
    pin(rop);
    if (batch) {
      ByteBuffer b; Writer w(b);
      w.u32((uint32_t)writes.size());
      for (auto& wr : writes) { w.id(wr.first); w.u64(0); w.bytes(ByteSpan(wr.second.data(), wr.second.size())); }
      rop.encode(w);
      std::vector<size_t> back;
      for (size_t i = 0; i < op.outputs.size(); ++i)
        if (!(i < op.leave_remote.size() && op.leave_remote[i])) back.push_back(i);
      w.u32((uint32_t)back.size());
      for (size_t i : back) { const JobStage& js = job_stage_[op.outputs[i].memory]; w.id(js.remote); w.u64(0); w.u64(js.size); w.u8(1); }
      // A named kernel may run far past a tensor op's two minutes; a peer that
      // dies still ends the call (the link drops).
      auto r = via.call(nodeproto::ExecBatch, b, 6 * 3600 * 1000);
      if (!r.ok()) return fail(r.error());
      Reader rr(*r);
      (void)rr.u64(); (void)rr.str();
      const uint32_t nr = rr.u32();
      if (nr != back.size()) return fail(Error{Status::Corrupt, "ExecBatch answered " + std::to_string(nr) + " reads for " + std::to_string(back.size())});
      for (size_t i : back) {
        ByteSpan data = rr.bytes();
        if (!rr.ok()) return fail(Error{Status::Corrupt, "ExecBatch reply cut short"});
        const MemoryObject* o = fabric.lookup(op.outputs[i].memory);
        if (!o) continue;
        uint8_t* host = static_cast<uint8_t*>(o->base());
        const size_t got = std::min<size_t>(data.size(), o->size);
        std::memcpy(host, data.data(), got);
        if (got < o->size) std::memset(host + got, 0, o->size - got);
        fabric.touch(op.outputs[i].memory);
        const MemoryObject* o2 = fabric.lookup(op.outputs[i].memory);
        job_stage_[op.outputs[i].memory].generation = o2 ? o2->generation : ~0ull;
      }
      for (size_t i = 0; i < op.outputs.size(); ++i)
        if (i < op.leave_remote.size() && op.leave_remote[i]) {
          const MemoryObject* o = fabric.lookup(op.outputs[i].memory);
          if (o) job_stage_[op.outputs[i].memory].generation = o->generation;
        }
      return Ok();
    }
    ByteBuffer b; Writer w(b); rop.encode(w);
    // A named kernel may run far past a tensor op's two minutes; a peer that
    // dies still ends the call (the link drops).
    auto r = via.call(nodeproto::Exec, b, 6 * 3600 * 1000);
    if (!r.ok()) return fail(r.error());
    for (size_t i = 0; i < op.outputs.size(); ++i) {
      const MemoryObject* o = fabric.lookup(op.outputs[i].memory);
      if (!o) continue;
      JobStage& js = job_stage_[op.outputs[i].memory];
      if (i < op.leave_remote.size() && op.leave_remote[i]) {
        // left where it was made: the peer's copy is the one that counts, and
        // an op here that reads this buffer next must not overwrite it
        js.generation = o->generation;
        continue;
      }
      auto rd = read_back(via, js.remote, js.size, static_cast<uint8_t*>(o->base()));
      if (!rd.ok()) return fail(rd.error());
      fabric.touch(op.outputs[i].memory);
      // local and remote now hold the same bytes
      const MemoryObject* o2 = fabric.lookup(op.outputs[i].memory);
      js.generation = o2 ? o2->generation : ~0ull;
    }
    return Ok();
  }

 private:
  DeviceID id_;
  std::shared_ptr<RemoteNode> node_;
  RemoteDeviceInfo info_;
  std::string name_;
  DeviceType type_;
  DeviceCapacity cap_;
  std::atomic<uint32_t> busy_{0};
  hal::Mutex mu_;
  std::unordered_map<MemoryID, uint64_t, TypedIdHash<2>> uploaded_;
};

// A channel that lives on the remote kernel. The local region holds only the
// remote channel identity; publish and consume cross the link.
class NetworkTransport final : public Transport {
 public:
  NetworkTransport(std::shared_ptr<RemoteNode> node, std::string name) : node_(std::move(node)), name_(std::move(name)) {}
  const char* name() const override { return name_.c_str(); }
  uint64_t region_bytes(uint32_t, uint64_t) const override { return 64; }
  Result<void> format(MemoryFabric& fabric, const MemoryID& region, uint32_t capacity, uint64_t slot_bytes) override {
    ByteBuffer b; Writer w(b); w.u32(capacity); w.u64(slot_bytes);
    auto r = node_->call(nodeproto::ChanCreate, b); if (!r.ok()) return r.error();
    Reader rd(*r); Id128 remote = rd.id();
    ByteBuffer hdr; Writer hw(hdr); hw.u32(0x4E534F41u); hw.id(remote);
    return fabric.write(region, 0, hdr);
  }
  Result<ChannelID> remote_id(MemoryFabric& fabric, const MemoryID& region) {
    auto d = fabric.read(region, 0, 20); if (!d.ok()) return d.error();
    Reader r(*d); if (r.u32() != 0x4E534F41u) return Result<ChannelID>(Status::Corrupt, "network channel region");
    return ChannelID{r.id()};
  }
  Result<uint64_t> publish(MemoryFabric& fabric, const MemoryID& region, const StateFrame& f) override {
    auto ch = remote_id(fabric, region); if (!ch.ok()) return ch.error();
    ByteBuffer b; Writer w(b); w.id(*ch); w.id(f.state); w.u8((uint8_t)f.kind); w.u64(f.generation); w.id(f.publisher); w.bytes(f.payload);
    w.u32((uint32_t)f.memory.size()); for (auto& m : f.memory) w.id(m);
    auto r = node_->call(nodeproto::ChanPublish, b); if (!r.ok()) return r.error();
    Reader rd(*r); return rd.u64();
  }
  Result<StateFrame> consume(MemoryFabric& fabric, const MemoryID& region, ChannelCursor& cursor) override {
    auto ch = remote_id(fabric, region); if (!ch.ok()) return ch.error();
    ByteBuffer b; Writer w(b); w.id(*ch);
    auto r = node_->call(nodeproto::ChanConsume, b); if (!r.ok()) return r.error();
    Reader rd(*r); StateFrame f;
    f.sequence = rd.u64(); f.publisher = EntityID{rd.id()}; f.state = StateID{rd.id()}; f.kind = (StateKind)rd.u8(); f.generation = rd.u64(); f.published_ns = rd.u64();
    ByteSpan p = rd.bytes(); f.payload.assign(p.begin(), p.end());
    uint32_t n = rd.u32(); for (uint32_t i = 0; i < n && rd.ok(); ++i) f.memory.push_back(MemoryID{rd.id()});
    cursor.next_sequence = f.sequence + 1;
    return f;
  }
  Result<void> stats(MemoryFabric&, const MemoryID&, uint64_t& head, uint64_t& tail, uint64_t& dropped) override { head = tail = dropped = 0; return Ok(); }
 private:
  std::shared_ptr<RemoteNode> node_;
  std::string name_;
};

}  // namespace

Result<RemoteAttachment> attach_remote_node(Kernel& k, std::shared_ptr<RemoteNode> node) {
  RemoteAttachment a;
  a.address = node->address();
  for (auto& d : node->devices()) {
    // Remote devices that are themselves remote are not re-exported (no chains in phase 1).
    if (d.type == DeviceType::RemoteCPU || d.type == DeviceType::RemoteGPU || d.type == DeviceType::RemoteNPU) continue;
    auto dev = std::make_unique<RemoteDevice>(k.ids().next_as<DeviceID>(), node, d);
    a.devices.push_back(dev->name());
    k.devices().add(std::move(dev));
  }
  if (auto* p = k.memory().provider(MemoryType::Remote)) static_cast<RemoteMemoryProvider*>(p)->add(node);   // only this file registers Remote
  else k.memory().register_provider(std::make_unique<RemoteMemoryProvider>(k.host(), node));
  a.provides_memory = true;
  a.transport = "network:" + node->address();
  k.channels().register_transport(std::make_unique<NetworkTransport>(node, a.transport));
  return a;
}

Result<std::string> choose_remote_memory(Kernel& k, size_t node_index) {
  if (node_index >= k.nodes().size()) return Result<std::string>(Status::NotFound, "no peer node #" + std::to_string(node_index) + " (" + std::to_string(k.nodes().size()) + " attached)");
  auto* p = k.memory().provider(MemoryType::Remote);
  if (!p) return Result<std::string>(Status::Unsupported, "no attached peer provides remote memory");
  auto node = k.nodes()[node_index];
  static_cast<RemoteMemoryProvider*>(p)->choose(node);
  return node->address();
}

std::string remote_memory_node(Kernel& k) {
  auto* p = k.memory().provider(MemoryType::Remote);
  if (!p) return "";
  auto n = static_cast<RemoteMemoryProvider*>(p)->chosen();
  return n ? n->address() : "";
}

}  // namespace aios
