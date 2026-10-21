#include "aios/userland/directive.hpp"

namespace aios::userland {
namespace {

// The register is a state object like any other, and it is recognised by what
// it starts with — the same way the git register is.
const char* const kMagic = "AIOSDIR1";

void put_u64(ByteBuffer& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
uint64_t get_u64(ByteSpan b, size_t& at) {
  uint64_t v = 0;
  for (int i = 0; i < 8 && at < b.size(); ++i, ++at) v |= (uint64_t)b[at] << (8 * i);
  return v;
}
void put_text(ByteBuffer& b, const std::string& s) {
  put_u64(b, s.size());
  b.insert(b.end(), s.begin(), s.end());
}
std::string get_text(ByteSpan b, size_t& at) {
  uint64_t n = get_u64(b, at);
  std::string s;
  for (uint64_t i = 0; i < n && at < b.size(); ++i, ++at) s += (char)b[at];
  return s;
}

bool is_register(ByteSpan b) {
  if (b.size() < 8) return false;
  for (int i = 0; i < 8; ++i) if (b[i] != (uint8_t)kMagic[i]) return false;
  return true;
}

}  // namespace

Directives::Directives(Surface& actor) : me_(actor) {}

std::string Directives::register_path() const { return me_.kernel().config().root + "/directives/register"; }

ByteBuffer Directives::encode() const {
  ByteBuffer b(kMagic, kMagic + 8);
  put_u64(b, directives_.size());
  for (const auto& d : directives_) {
    put_text(b, d.name);
    put_text(b, d.line);
    put_u64(b, d.every_ms);
    put_u64(b, d.due_ms);
    put_u64(b, d.runs);
    put_u64(b, d.last_ms);
    put_u64(b, d.last_took_ms);
    put_u64(b, d.enabled ? 1 : 0);
    put_u64(b, d.last_ok ? 1 : 0);
    put_text(b, d.last_result);
    put_u64(b, d.as.hi);
    put_u64(b, d.as.lo);
  }
  return b;
}

void Directives::decode(ByteSpan b) {
  directives_.clear();
  if (!is_register(b)) return;
  size_t at = 8;
  uint64_t n = get_u64(b, at);
  for (uint64_t i = 0; i < n && at < b.size(); ++i) {
    Directive d;
    d.name = get_text(b, at);
    d.line = get_text(b, at);
    d.every_ms = get_u64(b, at);
    d.due_ms = get_u64(b, at);
    d.runs = get_u64(b, at);
    d.last_ms = get_u64(b, at);
    d.last_took_ms = get_u64(b, at);
    d.enabled = get_u64(b, at) != 0;
    d.last_ok = get_u64(b, at) != 0;
    d.last_result = get_text(b, at);
    d.as.hi = get_u64(b, at);
    d.as.lo = get_u64(b, at);
    directives_.push_back(d);
  }
}

Result<void> Directives::attach(bool create_if_missing) {
  if (auto r = me_.may(ResourceRef{ResourceKind::State, {}}, R_Read); !r.ok())
    return Result<void>(Status::Denied, "entity " + me_.self().short_str() +
                                            " may not reach the standing directives: " + r.error().str());
  for (auto& s : me_.kernel().states().list()) {
    if (!is_register(ByteSpan(s.payload.data(), s.payload.size()))) continue;
    auto got = me_.state(s.id);                       // capability-checked read
    if (!got.ok()) return Result<void>(got.error());
    state_ = s.id;
    holder_ = s.owner;
    adopted_ = true;
    decode(ByteSpan(got->payload.data(), got->payload.size()));
    return Ok();
  }
  auto disk = me_.kernel().host().read_file(register_path());
  if (disk.ok()) decode(ByteSpan(disk->data(), disk->size()));
  if (!create_if_missing) {
    // Nothing was minted, and nothing had to be: a reader that finds no
    // register has simply found that this machine has no standing orders.
    return directives_.empty() ? Result<void>(Status::NotFound, "this machine holds no standing directives") : Ok();
  }
  restore_missing();
  ByteBuffer payload = encode();
  auto id = me_.create_state(StateKind::Persistent, ByteSpan(payload.data(), payload.size()));
  if (!id.ok()) return Result<void>(id.error());
  state_ = *id;
  holder_ = me_.self();
  adopted_ = false;
  return Ok();
}

std::string Directives::authority_path(const EntityID& who) const {
  return me_.kernel().config().root + "/directives/authority-" + who.str() + ".aoc";
}

// Whatever a standing directive acts as, brought back into this kernel if it is
// not here. A directive that names an entity nobody restored is an order with
// nobody to carry it out.
Result<size_t> Directives::restore_missing() {
  size_t brought_back = 0;
  for (const auto& d : directives_) {
    if (d.as.is_null()) continue;
    bool present = false;
    for (const auto& e : me_.kernel().entities().list())
      if (e.id == d.as) { present = true; break; }
    if (present) continue;
    auto path = authority_path(d.as);
    if (!me_.kernel().host().exists(path)) continue;
    auto back = me_.restore(path);
    if (!back.ok()) continue;
    ++brought_back;
  }
  return brought_back;
}

Result<void> Directives::commit() {
  ByteBuffer p = encode();
  if (!state_.is_null())
    if (auto r = me_.set_state(state_, ByteSpan(p.data(), p.size())); !r.ok()) return r;
  me_.kernel().host().make_dir(me_.kernel().config().root + "/directives");
  return me_.kernel().host().write_file(register_path(), ByteSpan(p.data(), p.size()));
}

const Directive* Directives::find(const std::string& name) const {
  for (const auto& d : directives_) if (d.name == name) return &d;
  return nullptr;
}

Result<void> Directives::add(const Directive& d) {
  if (d.name.empty()) return Result<void>(Status::Invalid, "a directive needs a name to be given or withdrawn by");
  if (d.line.empty()) return Result<void>(Status::Invalid, "a directive with nothing to do is not a directive");
  if (d.as.is_null())
    return Result<void>(Status::Invalid,
                        "a directive must name the entity whose authority it acts with — nothing here runs as the machine");
  // The order outlives this process only if the authority does. Capture it now,
  // beside the register, and the next kernel to read the register can restore
  // the same identity rather than inventing a new one.
  if (auto img = me_.checkpoint_of(d.as); img.ok()) {
    auto bytes = me_.kernel().host().read_file(*img);
    if (bytes.ok()) {
      me_.kernel().host().make_dir(me_.kernel().config().root + "/directives");
      me_.kernel().host().write_file(authority_path(d.as), ByteSpan(bytes->data(), bytes->size()));
    }
  }
  for (auto& existing : directives_)
    if (existing.name == d.name) { existing = d; return commit(); }
  directives_.push_back(d);
  return commit();
}

Result<void> Directives::remove(const std::string& name) {
  for (auto it = directives_.begin(); it != directives_.end(); ++it)
    if (it->name == name) { directives_.erase(it); return commit(); }
  return Result<void>(Status::NotFound, "no standing directive is called " + name);
}

Result<void> Directives::set_enabled(const std::string& name, bool on) {
  for (auto& d : directives_)
    if (d.name == name) { d.enabled = on; return commit(); }
  return Result<void>(Status::NotFound, "no standing directive is called " + name);
}

std::vector<Directive> Directives::due(uint64_t now_ms) const {
  std::vector<Directive> out;
  for (const auto& d : directives_)
    if (d.enabled && now_ms >= d.due_ms) out.push_back(d);
  return out;
}

Result<void> Directives::note_run(const std::string& name, uint64_t now_ms, uint64_t took_ms, bool ok,
                                  const std::string& result) {
  for (auto& d : directives_) {
    if (d.name != name) continue;
    d.runs++;
    d.last_ms = now_ms;
    d.last_took_ms = took_ms;
    d.last_ok = ok;
    d.last_result = result.size() > 160 ? result.substr(0, 160) : result;
    // A directive with no interval was a one-off: it is done, and says so
    // rather than disappearing.
    if (d.every_ms == 0) d.enabled = false;
    else d.due_ms = now_ms + d.every_ms;
    return commit();
  }
  return Result<void>(Status::NotFound, "no standing directive is called " + name);
}

}  // namespace aios::userland
