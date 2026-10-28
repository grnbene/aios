// grain, stage four: the machine that runs the bytecode.
//
// A stack machine with call frames and real closures. Two things here are not
// the textbook shape and both are deliberate:
//
//   Upvalues hold a stack INDEX, not a pointer. The value stack is a vector
//   and a vector moves when it grows; a captured pointer into it is a
//   use-after-free that only shows up in the one program deep enough to
//   reallocate mid-call.
//
//   Every instruction that can fail returns a message and a line rather than
//   trapping. The REPL has to survive a bad expression and offer the next
//   prompt, and a dream that divides by zero must not take the compositor with
//   it.
#include <cstddef>
#include <atomic>
#include <deque>
#include "aios/userland/grain.hpp"
#include "grain_jit.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#if !defined(__EMSCRIPTEN__)
#include <execinfo.h>
namespace { inline int native_stack(void** pc, int n) { return backtrace(pc, n); } }
#else
namespace { inline int native_stack(void**, int) { return 0; } }   // no machine stack to walk in WebAssembly
#endif
#include <map>
#include <sys/time.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <unordered_set>

namespace aios::userland::grain {

// --- values ----------------------------------------------------------------

Value Value::string(std::string s) {
  return Value::object(Type::Str, std::make_shared<StrObj>(std::move(s)));
}

Value Value::list(std::vector<Value> v) {
  return Value::object(Type::List, std::make_shared<ListObj>(std::move(v)));
}

Value Value::bytes(std::vector<uint8_t> v) {
  return Value::object(Type::Bytes, std::make_shared<BytesObj>(std::move(v)));
}

Value Value::id128(const aios::Id128& id) {
  return Value::object(Type::Id128, std::make_shared<Id128Obj>(id));
}

Value Value::id128(const std::string& hex) {
  aios::Id128 id;
  if (!aios::Id128::parse(hex, id)) {
    std::string padded = hex;
    if (padded.rfind("0x", 0) == 0 || padded.rfind("0X", 0) == 0) padded = padded.substr(2);
    while (padded.size() < 32) padded.push_back('0');
    if (!aios::Id128::parse(padded, id)) {
      id = aios::Id128{0, 0};
    }
  }
  return Value::id128(id);
}

const aios::Id128& Value::as_id128() const {
  static const aios::Id128 kNullId{};
  if (type != Type::Id128 || !obj) return kNullId;
  return static_cast<const Id128Obj*>(obj.get())->id;
}

bool ChannelObj::publish(Value payload, aios::Id128 publisher, aios::Id128 frame_id) {
  if (frame_id.is_null()) {
    static std::atomic<uint64_t> frame_seq{1};
    frame_id = aios::Id128{id.hi, ++frame_seq};
  }
  uint64_t ts = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
  auto frame = std::make_shared<FrameObj>(frame_id, ++generation, publisher, std::move(payload), ts);
  if (ring.size() >= capacity) {
    ring.erase(ring.begin());
    ++dropped;
    ++tail;
  }
  ring.push_back(std::move(frame));
  ++head;
  return true;
}

std::shared_ptr<FrameObj> ChannelObj::consume() {
  if (ring.empty()) return nullptr;
  auto f = ring.front();
  ring.erase(ring.begin());
  ++tail;
  return f;
}

bool MemObj::write(size_t offset, const std::string& data) {
  if (offset + data.size() > buffer.size()) {
    buffer.resize(offset + data.size(), 0);
    size = buffer.size();
  }
  std::memcpy(buffer.data() + offset, data.data(), data.size());
  ++generation;
  return true;
}

std::string MemObj::read(size_t offset, size_t len) const {
  if (offset >= buffer.size()) return "";
  size_t actual_len = std::min(len, buffer.size() - offset);
  return std::string(reinterpret_cast<const char*>(buffer.data() + offset), actual_len);
}

bool MemObj::migrate(const std::string& new_provider) {
  provider = new_provider;
  ++generation;
  return true;
}

Value EntityObj::get_field(const std::string& k) const {
  if (k == "id" || k == "identity") return Value::id128(id);
  if (k == "name") return Value::string(name);
  auto it = fields.find(k);
  if (it != fields.end()) return it->second;
  auto hit = handlers.find(k);
  if (hit != handlers.end()) return hit->second;
  return Value::nil();
}

void EntityObj::set_field(const std::string& k, Value v) {
  fields[k] = std::move(v);
}

namespace {
const std::string kEmpty;
std::vector<Value> g_empty_items;
}  // namespace

const std::string& Value::str() const {
  if (type != Type::Str || !obj) return kEmpty;
  return static_cast<StrObj*>(obj.get())->text;
}

std::vector<uint8_t>& Value::bytes_data() const {
  static std::vector<uint8_t> empty;
  if (type != Type::Bytes || !obj) { empty.clear(); return empty; }
  return static_cast<BytesObj*>(obj.get())->data;
}

std::vector<Value>& Value::list_items() const {
  if (type != Type::List || !obj) return g_empty_items;
  return static_cast<ListObj*>(obj.get())->items;
}

uint32_t MapObj::hash_of(const std::string& k) {
  return (uint32_t)std::hash<std::string_view>{}(std::string_view(k));
}

// The table is rebuilt from the cached hashes: never from the strings.
void MapObj::rebuild_slots(size_t at_least) {
  size_t n = 32;
  while (n < at_least * 2) n <<= 1;
  slots.assign(n, 0);
  const size_t mask = n - 1;
  for (size_t pos = 0; pos < hashes.size(); ++pos) {
    size_t i = hashes[pos] & mask;
    while (slots[i]) i = (i + 1) & mask;
    slots[i] = (uint32_t)(pos + 1);
  }
}

Value* MapObj::find(const std::string& k) {
  if (slots.empty()) {
    const size_t n = k.size();
    for (auto& e : entries)
      if (e.first.size() == n && std::memcmp(e.first.data(), k.data(), n) == 0) return &e.second;
    return nullptr;
  }
  const uint32_t h = hash_of(k);
  const size_t mask = slots.size() - 1;
  for (size_t i = h & mask;; i = (i + 1) & mask) {
    const uint32_t s = slots[i];
    if (!s) return nullptr;
    const size_t pos = s - 1;
    if (hashes[pos] == h && entries[pos].first == k) return &entries[pos].second;
  }
}

void MapObj::put(const std::string& k, Value v) {
  if (slots.empty()) {
    if (Value* have = find(k)) { *have = std::move(v); return; }
    entries.emplace_back(k, std::move(v));
    if (entries.size() > kIndexFrom) {
      hashes.resize(entries.size());
      for (size_t i = 0; i < entries.size(); ++i) hashes[i] = hash_of(entries[i].first);
      rebuild_slots(entries.size());
    }
    return;
  }
  const uint32_t h = hash_of(k);
  size_t mask = slots.size() - 1;
  size_t i = h & mask;
  for (;; i = (i + 1) & mask) {
    const uint32_t s = slots[i];
    if (!s) break;
    const size_t pos = s - 1;
    if (hashes[pos] == h && entries[pos].first == k) { entries[pos].second = std::move(v); return; }
  }
  entries.emplace_back(k, std::move(v));
  hashes.push_back(h);
  if (entries.size() * 2 > slots.size()) { rebuild_slots(entries.size()); return; }
  slots[i] = (uint32_t)entries.size();
}

// Erasing shifts the entries after it, so the index (when there is one) has to
// be told. Maps are small and deletion is rare; keeping insertion order is
// worth more than an O(1) erase that leaves holes.
bool MapObj::erase(const std::string& k) {
  size_t at = entries.size();
  if (slots.empty()) {
    for (size_t i = 0; i < entries.size(); ++i) if (entries[i].first == k) { at = i; break; }
  } else if (find(k)) {
    const uint32_t h = hash_of(k);
    for (size_t pos = 0; pos < entries.size(); ++pos)
      if (hashes[pos] == h && entries[pos].first == k) { at = pos; break; }
  }
  if (at == entries.size()) return false;
  entries.erase(entries.begin() + (long)at);
  if (!slots.empty()) {
    hashes.erase(hashes.begin() + (long)at);   // the positions after it moved down: the table is rebuilt, not patched
    rebuild_slots(entries.size());
  }
  return true;
}

namespace {

// Integers print without a decimal point; reals always print with one, so a
// program never has to wonder which kind it is holding.
std::string number_text(const Value& v) {
  char buf[40];
  if (v.type == Type::Int) { std::snprintf(buf, sizeof buf, "%lld", (long long)v.i); return buf; }
  if (v.d == (double)(int64_t)v.d && std::fabs(v.d) < 1e15) {
    std::snprintf(buf, sizeof buf, "%.1f", v.d);
  } else {
    std::snprintf(buf, sizeof buf, "%.12g", v.d);
  }
  return buf;
}

std::string quote(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

}  // namespace

std::string Value::to_text() const {
  switch (type) {
    case Type::Nil: return "nil";
    case Type::Bool: return b ? "true" : "false";
    case Type::Int: case Type::Real: return number_text(*this);
    case Type::Decimal: return decimal_text(*this);
    case Type::Str: return str();
    case Type::List: {
      std::string out = "[";
      const auto& items = list_items();
      for (size_t k = 0; k < items.size(); ++k) { if (k) out += ", "; out += items[k].repr(); }
      return out + "]";
    }
    case Type::Map: {
      std::string out = "{";
      auto* m = static_cast<MapObj*>(obj.get());
      for (size_t k = 0; k < m->entries.size(); ++k) {
        if (k) out += ", ";
        out += m->entries[k].first + ": " + m->entries[k].second.repr();
      }
      return out + "}";
    }
    case Type::Fn: {
      if (auto* c = dynamic_cast<ClosureObj*>(obj.get())) return "<fn " + c->fn->name + ">";
      if (auto* f = dynamic_cast<FnObj*>(obj.get())) return "<fn " + f->name + ">";
      return "<fn>";
    }
    case Type::Native: return "<native " + static_cast<NativeObj*>(obj.get())->name + ">";
    case Type::Extern: {
      auto* e = static_cast<ExternObj*>(obj.get());
      // A closed handle still prints, and says so: a program that logs one
      // after closing it should not be told it is looking at a live socket.
      return "<" + e->kind_name + (e->ptr ? "" : " closed") + ">";
    }
    case Type::Id128: return as_id128().str();
    case Type::Capability: {
      auto* c = static_cast<CapObj*>(obj.get());
      return "<capability " + c->id.short_str() + " " + c->resource + " rights=" + std::to_string(c->rights) + ">";
    }
    case Type::Channel: {
      auto* ch = static_cast<ChannelObj*>(obj.get());
      return "<channel " + ch->name + " id=" + ch->id.short_str() + " gen=" + std::to_string(ch->generation) + ">";
    }
    case Type::StateFrame: {
      auto* f = static_cast<FrameObj*>(obj.get());
      return "<frame gen=" + std::to_string(f->generation) + " payload=" + f->payload.to_text() + ">";
    }
    case Type::MemoryObject: {
      auto* m = static_cast<MemObj*>(obj.get());
      return "<memory " + m->id.short_str() + " " + std::to_string(m->size) + "B (" + m->provider + ")>";
    }
    case Type::Entity: {
      auto* ent = static_cast<EntityObj*>(obj.get());
      return "<entity " + ent->name + " id=" + ent->id.short_str() + ">";
    }
    case Type::BoundMethod: {
      auto* bm = static_cast<BoundMethodObj*>(obj.get());
      return "<method " + bm->name + ">";
    }
    case Type::Bytes: {
      // <bytes 5: 68 65 6c 6c 6f>, the first 32 shown
      const auto& data = static_cast<BytesObj*>(obj.get())->data;
      static const char* hex = "0123456789abcdef";
      std::string out = "<bytes " + std::to_string(data.size()) + ":";
      for (size_t k = 0; k < data.size() && k < 32; ++k) { out += ' '; out += hex[data[k] >> 4]; out += hex[data[k] & 15]; }
      if (data.size() > 32) out += " ...";
      return out + ">";
    }
  }
  return "?";
}

// Inside a container a string has to be quoted, or ["a, b"] and ["a", "b"]
// print the same and neither can be read back.
std::string Value::repr() const {
  if (type == Type::Str) return quote(str());
  if (type == Type::Id128) return "id128(\"" + as_id128().str() + "\")";
  return to_text();
}

bool Value::equals(const Value& o) const {
  // 2 and 2.0 are the same number written two ways. Everything else has to
  // match kind as well as content.
  if (is_number() && o.is_number()) {
    if (type == Type::Int && o.type == Type::Int) return i == o.i;
    return as_double() == o.as_double();
  }
  // A decimal equals an int or decimal of the same value (2.50 == 2.5 == ... 2.5); never a real, which is only approximately anything.
  if (is_exact_number(*this) && is_exact_number(o)) return decimal_compare(*this, o) == 0;
  if (type != o.type) return false;
  switch (type) {
    case Type::Nil: return true;
    case Type::Bool: return b == o.b;
    case Type::Str: return str() == o.str();
    case Type::Id128: return as_id128() == o.as_id128();
    case Type::Capability:
      return static_cast<CapObj*>(obj.get())->id == static_cast<CapObj*>(o.obj.get())->id;
    case Type::Channel:
      return static_cast<ChannelObj*>(obj.get())->id == static_cast<ChannelObj*>(o.obj.get())->id;
    case Type::StateFrame:
      return static_cast<FrameObj*>(obj.get())->id == static_cast<FrameObj*>(o.obj.get())->id;
    case Type::MemoryObject:
      return static_cast<MemObj*>(obj.get())->id == static_cast<MemObj*>(o.obj.get())->id;
    case Type::Bytes:
      return static_cast<BytesObj*>(obj.get())->data == static_cast<BytesObj*>(o.obj.get())->data;
    case Type::Entity:
      return static_cast<EntityObj*>(obj.get())->id == static_cast<EntityObj*>(o.obj.get())->id;
    case Type::List: {
      const auto& x = list_items();
      const auto& y = o.list_items();
      if (x.size() != y.size()) return false;
      for (size_t k = 0; k < x.size(); ++k) if (!x[k].equals(y[k])) return false;
      return true;
    }
    case Type::Map: {
      auto* a = static_cast<MapObj*>(obj.get());
      auto* c = static_cast<MapObj*>(o.obj.get());
      if (!a || !c || a->entries.size() != c->entries.size()) return false;
      for (auto& [k, v] : a->entries) {
        Value* other = c->find(k);
        if (!other || !v.equals(*other)) return false;
      }
      return true;
    }
    default: return obj == o.obj;   // functions are the same if identical
  }
}

// --- disassembler ----------------------------------------------------------

namespace {

size_t print_one(std::ostringstream& out, const FnObj& fn, size_t at, int& last_line) {
  const Chunk& c = fn.chunk;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04zu ", at);
  out << buf;
  const int line = c.lines[at];
  if (line == last_line) out << "   | ";
  else { std::snprintf(buf, sizeof buf, "%4d ", line); out << buf; last_line = line; }

  const Op op = (Op)c.code[at];
  out << op_name(op);
  switch (op) {
    case Op::Constant: case Op::GetGlobal: case Op::SetGlobal: case Op::DefineGlobal:
    case Op::FieldGet: case Op::OptFieldGet: case Op::FieldSet: {
      const int k = (c.code[at + 1] << 8) | c.code[at + 2];
      out << " " << k << "  ; " << c.constants[k].repr() << "\n";
      return at + 3;
    }
    case Op::BuildList: case Op::BuildMap: case Op::ListConcat:
      out << " " << ((c.code[at + 1] << 8) | c.code[at + 2]) << "\n";
      return at + 3;
    case Op::GetLocal: case Op::SetLocal: case Op::GetUpvalue: case Op::SetUpvalue:
    case Op::Call: case Op::IterNext:
      out << " " << (int)c.code[at + 1] << "\n";
      return at + 2;
    case Op::Jump: case Op::JumpIfFalse: case Op::JumpIfTrue: {
      const int d = (c.code[at + 1] << 8) | c.code[at + 2];
      out << " -> " << (at + 3 + (size_t)d) << "\n";
      return at + 3;
    }
    case Op::Loop: {
      const int d = (c.code[at + 1] << 8) | c.code[at + 2];
      out << " -> " << (at + 3 - (size_t)d) << "\n";
      return at + 3;
    }
    case Op::Closure: {
      const int k = (c.code[at + 1] << 8) | c.code[at + 2];
      auto sub = std::static_pointer_cast<FnObj>(c.constants[k].obj);
      out << " " << k << "  ; fn " << (sub ? sub->name : "?") << "\n";
      size_t next = at + 3;
      const size_t n = sub ? sub->upvalue_index.size() : 0;
      for (size_t u = 0; u < n; ++u) {
        std::snprintf(buf, sizeof buf, "%04zu ", next);
        out << buf << "   |   capture " << (c.code[next] ? "local" : "upvalue")
            << " " << (int)c.code[next + 1] << "\n";
        next += 2;
      }
      return next;
    }
    default:
      out << "\n";
      return at + 1;
  }
}

void disassemble_into(std::ostringstream& out, const FnObj& fn) {
  out << "== fn " << fn.name << " (" << fn.arity << " param(s), "
      << fn.chunk.code.size() << " bytes, " << fn.chunk.constants.size() << " constant(s)) ==\n";
  int last_line = -1;
  for (size_t at = 0; at < fn.chunk.code.size();) at = print_one(out, fn, at, last_line);
  // Nested functions are constants of their parent, so printing them after it
  // gives the whole program in one listing rather than one per invocation.
  for (const Value& k : fn.chunk.constants) {
    if (k.type == Type::Fn && k.obj) {
      if (auto* sub = dynamic_cast<FnObj*>(k.obj.get())) { out << "\n"; disassemble_into(out, *sub); }
    }
  }
}

}  // namespace

std::string disassemble(const FnObj& fn) {
  std::ostringstream out;
  disassemble_into(out, fn);
  return out.str();
}

// --- the machine -----------------------------------------------------------

struct Vm::Frame {
  // Raw: the callee Value sits in stack slot `base` for the whole call (slot 0
  // is reserved for it), which keeps the closure alive without an atomic
  // refcount pair per call. `owned` is only set when the VM had to wrap a bare
  // FnObj in a fresh closure that nothing else holds.
  ClosureObj* closure = nullptr;
  size_t ip = 0;
  size_t base = 0;
  std::shared_ptr<ClosureObj> owned;
  const Chunk* chunk = nullptr;
  Frame(ClosureObj* c, size_t i, size_t b, std::shared_ptr<ClosureObj> o = nullptr)
      : closure(c), ip(i), base(b), owned(std::move(o)), chunk(&c->fn->chunk) {}
};

namespace {
// The closure a Fn value calls, without RTTI. A bare FnObj gets a new closure,
// returned through `owned` so the caller can keep it alive.
ClosureObj* closure_of(const Value& v, std::shared_ptr<ClosureObj>& owned) {
  if (v.type != Type::Fn || !v.obj) return nullptr;
  if (v.obj->is_closure) return static_cast<ClosureObj*>(v.obj.get());
  owned = std::make_shared<ClosureObj>();
  owned->fn = std::static_pointer_cast<FnObj>(v.obj);
  return owned.get();
}

// Where a global lives, through the chunk's inline cache.
Value* cached_global(const Chunk& chunk, uint16_t k, uint64_t owner,
                     std::unordered_map<std::string, Value>& globals) {
  if (chunk.global_cache_owner != owner) {
    chunk.global_cache.assign(chunk.constants.size(), nullptr);
    chunk.global_cache_owner = owner;
  } else if (chunk.global_cache.size() < chunk.constants.size()) {
    chunk.global_cache.resize(chunk.constants.size(), nullptr);
  }
  Value*& slot = chunk.global_cache[k];
  if (!slot) {
    auto it = globals.find(chunk.constants[k].str());
    if (it != globals.end()) slot = &it->second;
  }
  return slot;
}
}  // namespace

struct Vm::State {
  std::vector<Value> stack;
  std::vector<Frame> frames;
  std::vector<std::shared_ptr<Upvalue>> open;
  std::unordered_map<std::string, Value> globals;
  uint64_t serial = next_serial();
  jit::Ctx jit_ctx{};
  std::deque<std::vector<Value>> native_args;    // see call_value: one per native nesting depth (deque: growing it must not move the ones in use)
  size_t native_depth = 0;
  std::vector<std::weak_ptr<Obj>> tracked_containers;
  size_t gc_prune_at = 4096;
  bool in_gc = false;
  static uint64_t next_serial() { static std::atomic<uint64_t> n{0}; return ++n; }
  std::string error;
  int error_line = 0;
  int error_column = 0;
};

Vm::Vm() : st_(std::make_unique<State>()) { st_->stack.reserve(1024); st_->frames.reserve(256); }
Vm::~Vm() {
  if (st_) {
    timers.clear();
    type_names.clear();
    st_->globals.clear();
    st_->stack.clear();
    st_->frames.clear();
    st_->open.clear();
    collect_cycles();
  }
}

bool Vm::retire_heap() {
  tasks.reset();                                   // waits for any spawn() not awaited, as dropping the VM does
  if (ExternObj::live.load() != 0) return false;   // something holds a handle that is closed by being freed
  (void)st_.release();                             // the stack, globals and every object they reach: left to the OS
  return true;
}

void Vm::define_global(const std::string& name, Value v) { st_->globals[name] = std::move(v); }
Value* Vm::find_global(const std::string& name) {
  auto it = st_->globals.find(name);
  return it == st_->globals.end() ? nullptr : &it->second;
}

uint64_t Vm::serial() const { return st_->serial; }

Value Vm::upvalue_value(const Upvalue& uv) const {
  if (uv.is_closed) return uv.closed;
  return uv.slot < st_->stack.size() ? st_->stack[uv.slot] : Value::nil();
}

// The cycle collector only ever looks at lists, maps, closures and entities, and
// looks at each once: a flag on the object keeps a hot loop that writes fields
// from queueing the same map a million times.
void Vm::track_container(const std::shared_ptr<Obj>& obj) {
  if (!obj || obj->gc_tracked) return;
  const bool container = obj->kind == Type::List || obj->kind == Type::Map || obj->kind == Type::Entity ||
                         (obj->kind == Type::Fn && obj->is_closure);
  if (!container) return;
  obj->gc_tracked = true;
  auto& list = st_->tracked_containers;
  list.push_back(obj);
  if (list.size() >= st_->gc_prune_at) {
    collect_cycles();
  }
}

// Breaks reference cycles that nothing outside them can reach, and returns how many
// objects it emptied. Plain trial deletion: an object is "held from outside" when
// more references point at it than the tracked objects themselves account for.
// Those are the roots - and because that is counted from the references themselves,
// a list a native function is in the middle of walking, a value another VM holds, or
// a bound method's receiver all count as outside without the collector having to
// know where they live. What no root reaches is cyclic garbage; its contents are
// cleared and the ordinary reference counts free it.
size_t Vm::collect_cycles() {
  if (st_->in_gc) return 0;
  st_->in_gc = true;
  struct InGcGuard { bool& f; ~InGcGuard() { f = false; } } guard{st_->in_gc};

  auto& tracked = st_->tracked_containers;
  if (tracked.empty()) return 0;

  std::vector<std::shared_ptr<Obj>> nodes;
  nodes.reserve(tracked.size());
  for (auto& w : tracked) if (auto sp = w.lock()) nodes.push_back(std::move(sp));
  struct PtrIndex {
    struct Slot { Obj* p = nullptr; uint32_t idx = 0; };
    std::vector<Slot> slots;
    size_t mask = 0;

    void build(const std::vector<std::shared_ptr<Obj>>& nodes) {
      size_t cap = 16;
      while (cap < nodes.size() * 2) cap <<= 1;
      slots.assign(cap, Slot{nullptr, 0});
      mask = cap - 1;
      for (size_t i = 0; i < nodes.size(); ++i) {
        Obj* p = nodes[i].get();
        uintptr_t v = reinterpret_cast<uintptr_t>(p);
        size_t h = (v >> 4) ^ (v >> 9);
        size_t s = h & mask;
        while (slots[s].p != nullptr) s = (s + 1) & mask;
        slots[s].p = p;
        slots[s].idx = static_cast<uint32_t>(i);
      }
    }

    int32_t find(Obj* p) const {
      if (slots.empty() || !p) return -1;
      uintptr_t v = reinterpret_cast<uintptr_t>(p);
      size_t h = (v >> 4) ^ (v >> 9);
      size_t s = h & mask;
      while (slots[s].p != nullptr) {
        if (slots[s].p == p) return static_cast<int32_t>(slots[s].idx);
        s = (s + 1) & mask;
      }
      return -1;
    }
  } index;
  index.build(nodes);

  // every reference a tracked object holds to another tracked object
  auto each_edge = [&](Obj* o, auto&& visit) {
    // Only a reference a tracked object holds itself counts as inside. A bound method's receiver is held by the
    // bound-method object, which other values can share, so it stays "held from outside": never collected from
    // under whoever still has the method.
    auto val = [&](const Value& v) { if (v.obj) visit(v.obj.get()); };
    if (o->kind == Type::List) {
      for (const auto& x : static_cast<ListObj*>(o)->items) val(x);
    } else if (o->kind == Type::Map) {
      for (const auto& e : static_cast<MapObj*>(o)->entries) val(e.second);
    } else if (o->kind == Type::Fn && o->is_closure) {
      std::unordered_set<const Upvalue*> seen;       // a shared upvalue holds its value once
      for (const auto& u : static_cast<ClosureObj*>(o)->upvalues)
        if (u && u->is_closed && seen.insert(u.get()).second) val(u->closed);
    } else if (o->kind == Type::Entity) {
      auto* e = static_cast<EntityObj*>(o);
      for (const auto& f : e->fields) val(f.second);
      for (const auto& h : e->handlers) val(h.second);
    }
  };

  std::vector<long> held(nodes.size());
  for (size_t i = 0; i < nodes.size(); ++i)
    held[i] = nodes[i].use_count() - 1;              // minus the reference `nodes` itself holds
  for (size_t i = 0; i < nodes.size(); ++i)
    each_edge(nodes[i].get(), [&](Obj* c) {
      int32_t idx = index.find(c);
      if (idx >= 0) --held[static_cast<size_t>(idx)];
    });

  std::vector<char> live(nodes.size(), 0);
  std::vector<size_t> work;
  for (size_t i = 0; i < nodes.size(); ++i) if (held[i] > 0) { live[i] = 1; work.push_back(i); }
  while (!work.empty()) {
    const size_t i = work.back(); work.pop_back();
    each_edge(nodes[i].get(), [&](Obj* c) {
      int32_t idx = index.find(c);
      if (idx >= 0 && !live[static_cast<size_t>(idx)]) {
        live[static_cast<size_t>(idx)] = 1;
        work.push_back(static_cast<size_t>(idx));
      }
    });
  }

  size_t collected = 0;
  for (size_t i = 0; i < nodes.size(); ++i) {
    if (live[i]) continue;
    ++collected;
    Obj* o = nodes[i].get();
    if (o->kind == Type::Map) { auto* m = static_cast<MapObj*>(o); m->clear_all(); }
    else if (o->kind == Type::List) static_cast<ListObj*>(o)->items.clear();
    else if (o->kind == Type::Fn && o->is_closure) static_cast<ClosureObj*>(o)->upvalues.clear();
    else if (o->kind == Type::Entity) { auto* e = static_cast<EntityObj*>(o); e->fields.clear(); e->handlers.clear(); }
  }

  tracked.clear();
  for (size_t i = 0; i < nodes.size(); ++i) if (live[i]) tracked.push_back(nodes[i]);
  st_->gc_prune_at = std::max<size_t>(8192, tracked.size() * 3);
  return collected;
}

size_t Vm::frame_depth() const { return st_->frames.size(); }

std::vector<std::string> Vm::stack_trace() const {
  std::vector<std::string> res;
  for (auto it = st_->frames.rbegin(); it != st_->frames.rend(); ++it) {
    std::string name = (it->closure && it->closure->fn) ? it->closure->fn->name : "<unknown>";
    int line = 0;
    if (it->chunk && it->ip < it->chunk->lines.size()) line = it->chunk->lines[it->ip];
    res.push_back(name + " at line " + std::to_string(line));
  }
  return res;
}

std::vector<std::pair<std::string, Value>> Vm::current_locals() const {
  std::vector<std::pair<std::string, Value>> res;
  if (st_->frames.empty()) return res;
  const Frame& fr = st_->frames.back();
  if (!fr.closure || !fr.closure->fn) return res;
  for (const auto& d : fr.closure->fn->debug_locals) {
    if (fr.ip < d.from || fr.ip >= d.to) continue;           // not in scope here
    const size_t at = fr.base + (size_t)d.slot;
    if (at < st_->stack.size()) res.push_back({d.name, st_->stack[at]});
  }
  return res;
}

std::vector<std::string> Vm::global_names() const {
  std::vector<std::string> names;
  names.reserve(st_->globals.size());
  for (const auto& g : st_->globals) names.push_back(g.first);
  std::sort(names.begin(), names.end());
  return names;
}

void Vm::retain_globals(const std::vector<std::string>& keep) {
  for (auto it = st_->globals.begin(); it != st_->globals.end();) {
    if (std::find(keep.begin(), keep.end(), it->first) == keep.end()) it = st_->globals.erase(it);
    else ++it;
  }
}

void Vm::define_native(const std::string& name, int min_args, int max_args, NativeFn fn) {
  st_->globals[name] = Value::object(Type::Native,
      std::make_shared<NativeObj>(name, min_args, max_args, fn));
}

void Vm::emit(const std::string& line) {
  if (on_print) { on_print(line); return; }
  if (sink) { sink->push_back(line); return; }
  // Flushed per line. A server's log is read while the server is still
  // running, and a buffered stream redirected to a file shows nothing at all
  // until the program ends -- which for a server is never.
  std::cout << line << "\n" << std::flush;
}

namespace {

bool numeric_pair(const Value& a, const Value& b) { return a.is_number() && b.is_number(); }
// A decimal on either side with an int or decimal on the other.
bool decimal_pair(const Value& a, const Value& b) {
  return (a.type == Type::Decimal || b.type == Type::Decimal) && is_exact_number(a) && is_exact_number(b);
}
bool both_int(const Value& a, const Value& b) { return a.type == Type::Int && b.type == Type::Int; }

// Push a copy of `src` without touching its shared_ptr when it has none -
// numbers, bools and nil are most of what a loop moves. The fields are read
// before the push because the push may reallocate the stack `src` lives in.
inline void push_copy(std::vector<Value>& stack, const Value& src) {
  if (src.obj) { stack.push_back(src); return; }
  const Type t = src.type; const int64_t bits = src.i;
  stack.emplace_back();
  Value& d = stack.back();
  d.type = t; d.i = bits;
}

}  // namespace

FnObj::~FnObj() { jit::release(jit_code); }

// Cycle accounting for the native call path (Pi only: the PMU cycle counter
// is readable at EL1). `grain prof` prints and resets it.
JitProfile g_jit_profile;
#if defined(AIOS_METAL) && defined(__aarch64__)
static inline uint64_t prof_now() { uint64_t v; asm volatile("mrs %0, pmccntr_el0" : "=r"(v)); return v; }
#define PROF_BEGIN() const uint64_t prof_t0 = prof_now()
#define PROF_END(field) (worker ? void() : void((g_jit_profile.field += prof_now() - prof_t0, ++g_jit_profile.field##_n)))
// One counter for the whole process, so only the program's own thread counts: a worker VM
// (parallel_map, spawn) runs on another core at the same time.
#define PROF_COUNT(expr) (worker ? void() : void(expr))
#else
#define PROF_BEGIN() do {} while (0)
#define PROF_END(field) do {} while (0)
#define PROF_COUNT(expr) do {} while (0)   // nothing reads the counters off the Pi
#endif

bool Vm::step_one(size_t ip) {
  PROF_BEGIN();
  st_->frames.back().ip = ip;
  PROF_COUNT(++g_jit_profile.step_by_op[st_->frames.back().chunk->code[ip] & 63]);
  const bool ok = execute_loop(0, true);
  PROF_END(step);
  return ok;
}

// The callee's frame is already pushed; run its native code to its Return.

// GRAIN_PROFILE=1: a 1 kHz CPU-time sample of the source line being run, printed to stderr at exit. A builtin's
// time lands on the line that called it. Off, the cost is one never-taken branch per instruction.
namespace {
struct ProfSample { int line; int depth; void* pc[14]; };
bool g_prof_on = false;
bool g_prof_native = false;
const Chunk* volatile g_prof_chunk = nullptr;
volatile size_t g_prof_ip = 0;
ProfSample g_prof_buf[1 << 17];
volatile size_t g_prof_n = 0;
void prof_tick(int) {
  const Chunk* c = g_prof_chunk;
  const size_t n = g_prof_n;
  const size_t ip = g_prof_ip;
  if (c && n < (sizeof g_prof_buf / sizeof g_prof_buf[0])) {
    const size_t at = ip > 0 ? ip - 1 : 0;
    ProfSample& ps = g_prof_buf[n];
    ps.line = at < c->lines.size() ? c->lines[at] : 0;
    ps.depth = g_prof_native ? native_stack(ps.pc, 14) : 0;
    g_prof_n = n + 1;
  }
}
void prof_report() {
  std::map<int, size_t> by_line;
  for (size_t i = 0; i < g_prof_n; ++i) {
    by_line[g_prof_buf[i].line]++;
  }
  std::vector<std::pair<size_t, int>> top;
  for (auto& kv : by_line) top.push_back({kv.second, kv.first});
  std::sort(top.rbegin(), top.rend());
  if (g_prof_native) {
    // GRAIN_PROFILE=native: every sample's call stack, innermost first, as hex addresses (one line each) to GRAIN_PROFILE_OUT
    const char* out = std::getenv("GRAIN_PROFILE_OUT");
    if (FILE* f = std::fopen(out ? out : "grain-profile.stacks", "w")) {
      for (size_t i = 0; i < g_prof_n; ++i) {
        for (int d = 0; d < g_prof_buf[i].depth; ++d) std::fprintf(f, "%p ", g_prof_buf[i].pc[d]);
        std::fprintf(f, "\n");
      }
      std::fclose(f);
    }
  }
  std::fprintf(stderr, "grain profile: %zu samples (ms)\n", (size_t)g_prof_n);
  for (size_t i = 0; i < top.size() && i < 40; ++i) std::fprintf(stderr, "  %6zu ms  line %d\n", top[i].first, top[i].second);
}
void prof_start() {
  static bool once = false;
  if (once) return;
  once = true;
  const char* want = std::getenv("GRAIN_PROFILE");
  if (!want) return;
  g_prof_on = true;
  g_prof_native = std::strcmp(want, "native") == 0;
  struct sigaction sa; std::memset(&sa, 0, sizeof sa);
  sa.sa_handler = prof_tick; sa.sa_flags = SA_RESTART;
  sigaction(SIGPROF, &sa, nullptr);
  itimerval it; it.it_interval.tv_sec = 0; it.it_interval.tv_usec = 1000; it.it_value = it.it_interval;
  setitimer(ITIMER_PROF, &it, nullptr);
  std::atexit(prof_report);
}
}  // namespace

bool Vm::run_native(void* code, size_t base) {
#if UINTPTR_MAX == UINT64_MAX  // the JITs are 64-bit; a 32-bit build (wasm) never runs native code
  static_assert(sizeof(Frame) == jit::kFrameSize, "the native call path writes Frames directly");
  static_assert(offsetof(Frame, closure) == jit::kFrameClosure && offsetof(Frame, ip) == jit::kFrameIp &&
                offsetof(Frame, base) == jit::kFrameBase && offsetof(Frame, owned) == jit::kFrameOwnedPtr &&
                offsetof(Frame, chunk) == jit::kFrameChunk, "Frame layout");
#endif
  State& S = *st_;
  S.jit_ctx.vm = this;
  S.jit_ctx.stack = &S.stack;
  S.jit_ctx.steps = &steps;
  S.jit_ctx.step_limit = &step_limit;
  S.jit_ctx.frames = &S.frames;
  S.jit_ctx.open = &S.open;
  S.jit_ctx.serial = S.serial;
  return reinterpret_cast<jit::Entry>(code)(&S.jit_ctx, base) != 0 && S.error.empty();
}

namespace {
void* native_for(FnObj& fn) {
  if (!fn.jit_tried) {
    fn.jit_tried = true;
    fn.jit_code = reinterpret_cast<void*>(jit::compile(fn));
  }
  return fn.jit_code;
}

Value method_channel_publish(Vm&, BoundMethodObj& bm, std::vector<Value>& args, std::string& error) {
  auto* ch = static_cast<ChannelObj*>(bm.receiver.obj.get());
  if (args.empty()) { error = "publish() expects a payload"; return Value::nil(); }
  aios::Id128 pub_id{};
  if (args.size() > 1 && args[1].is_id128()) pub_id = args[1].as_id128();
  ch->publish(args[0], pub_id);
  return Value::boolean(true);
}

Value method_channel_consume(Vm&, BoundMethodObj& bm, std::vector<Value>&, std::string&) {
  auto* ch = static_cast<ChannelObj*>(bm.receiver.obj.get());
  auto f = ch->consume();
  if (!f) return Value::nil();
  return Value::object(Type::StateFrame, f);
}

Value method_channel_stats(Vm&, BoundMethodObj& bm, std::vector<Value>&, std::string&) {
  auto* ch = static_cast<ChannelObj*>(bm.receiver.obj.get());
  auto m = std::make_shared<MapObj>();
  m->put("name", Value::string(ch->name));
  m->put("id", Value::id128(ch->id));
  m->put("capacity", Value::integer((int64_t)ch->capacity));
  m->put("generation", Value::integer((int64_t)ch->generation));
  m->put("head", Value::integer((int64_t)ch->head));
  m->put("tail", Value::integer((int64_t)ch->tail));
  m->put("dropped", Value::integer((int64_t)ch->dropped));
  return Value::object(Type::Map, m);
}

Value method_cap_attenuate(Vm&, BoundMethodObj& bm, std::vector<Value>& args, std::string& error) {
  auto* cap = static_cast<CapObj*>(bm.receiver.obj.get());
  if (args.empty() || args[0].type != Type::Int) { error = "attenuate() expects integer rights"; return Value::nil(); }
  uint16_t narrower = static_cast<uint16_t>(args[0].as_int());
  if ((narrower & ~cap->rights) != 0) {
    error = "capability attenuation error: cannot escalate rights";
    return Value::nil();
  }
  static std::atomic<uint64_t> cap_seq{1};
  aios::Id128 child_id{cap->id.hi, ++cap_seq};
  auto child = cap->attenuate(narrower, child_id);
  return Value::object(Type::Capability, child);
}

Value method_cap_can(Vm&, BoundMethodObj& bm, std::vector<Value>& args, std::string& error) {
  auto* cap = static_cast<CapObj*>(bm.receiver.obj.get());
  if (args.empty() || args[0].type != Type::Int) { error = "can() expects integer rights bitmask"; return Value::nil(); }
  return Value::boolean(cap->can(static_cast<uint16_t>(args[0].as_int())));
}

Value method_cap_has_expired(Vm&, BoundMethodObj& bm, std::vector<Value>& args, std::string&) {
  auto* cap = static_cast<CapObj*>(bm.receiver.obj.get());
  uint64_t gen = args.empty() ? 0 : (uint64_t)args[0].as_int();
  return Value::boolean(cap->has_expired(gen));
}

Value method_mem_write(Vm&, BoundMethodObj& bm, std::vector<Value>& args, std::string& error) {
  auto* mem = static_cast<MemObj*>(bm.receiver.obj.get());
  if (args.size() < 2 || args[0].type != Type::Int || args[1].type != Type::Str) {
    error = "write() expects (offset, string)";
    return Value::nil();
  }
  mem->write((size_t)args[0].as_int(), args[1].str());
  return Value::boolean(true);
}

Value method_mem_read(Vm&, BoundMethodObj& bm, std::vector<Value>& args, std::string& error) {
  auto* mem = static_cast<MemObj*>(bm.receiver.obj.get());
  if (args.size() < 2 || args[0].type != Type::Int || args[1].type != Type::Int) {
    error = "read() expects (offset, length)";
    return Value::nil();
  }
  return Value::string(mem->read((size_t)args[0].as_int(), (size_t)args[1].as_int()));
}

Value method_mem_migrate(Vm&, BoundMethodObj& bm, std::vector<Value>& args, std::string& error) {
  auto* mem = static_cast<MemObj*>(bm.receiver.obj.get());
  if (args.empty() || args[0].type != Type::Str) {
    error = "migrate() expects target provider string";
    return Value::nil();
  }
  mem->migrate(args[0].str());
  return Value::boolean(true);
}

Value method_entity_dispatch(Vm& vm, BoundMethodObj& bm, std::vector<Value>& args, std::string& error) {
  auto* ent = static_cast<EntityObj*>(bm.receiver.obj.get());
  if (args.empty() || args[0].type != Type::Str) {
    error = "dispatch() expects event name string";
    return Value::nil();
  }
  const std::string& event = args[0].str();
  auto it = ent->handlers.find(event);
  if (it == ent->handlers.end()) {
    return Value::nil();
  }
  Value event_arg = args.size() > 1 ? args[1] : Value::nil();
  return vm.call(it->second, {bm.receiver, event_arg}, error);
}

Value method_entity_handler_call(Vm& vm, BoundMethodObj& bm, std::vector<Value>& args, std::string& error) {
  auto* ent = static_cast<EntityObj*>(bm.receiver.obj.get());
  auto it = ent->handlers.find(bm.name);
  if (it == ent->handlers.end()) {
    return Value::nil();
  }
  Value event_arg = args.empty() ? Value::nil() : args[0];
  return vm.call(it->second, {bm.receiver, event_arg}, error);
}

}  // namespace

void Vm::close_upvalues_from(size_t from) {
  State& S = *st_;
  for (size_t i = S.open.size(); i-- > 0;) {
    if (S.open[i]->slot < from) continue;
    S.open[i]->closed = S.stack[S.open[i]->slot];
    S.open[i]->is_closed = true;
    S.open.erase(S.open.begin() + (long)i);
  }
}

// A call: `callee` and its argc arguments are the top of the stack. Natives
// run to completion here; a grain function gets its frame pushed and - when it
// has native code - runs to its Return before this returns.
bool Vm::invoke(const Value& callee, uint8_t argc, int line, int column) {
  State& S = *st_;
  auto fail = [&](int l, const std::string& msg) { S.error = msg; S.error_line = l; S.error_column = column; };
  if (callee.type == Type::Native) {
    auto* nat = static_cast<NativeObj*>(callee.obj.get());
    if (argc < nat->min_args || (nat->max_args >= 0 && argc > nat->max_args)) {
      fail(line, nat->name + "() takes " + std::to_string(nat->min_args) +
           (nat->max_args == nat->min_args ? "" : "-" + (nat->max_args < 0 ? std::string("any")
                                                         : std::to_string(nat->max_args))) +
           " argument(s), got " + std::to_string((int)argc));
      return false;
    }
    // Argument vectors come from a per-depth pool instead of a fresh heap
    // allocation per native call; depth-indexed because a native (map,
    // filter, each...) can call back into grain, which calls natives.
    if (S.native_depth >= S.native_args.size()) S.native_args.emplace_back();
    std::vector<Value>& args = S.native_args[S.native_depth];
    args.assign(std::make_move_iterator(S.stack.end() - argc), std::make_move_iterator(S.stack.end()));
    std::string err;
    ++S.native_depth;
    Value out = nat->fn(*this, args, err);
    --S.native_depth;
    args.clear();
    S.stack.resize(S.stack.size() - argc - 1);
    if (!err.empty()) { fail(line, err); return false; }
    S.stack.push_back(std::move(out));
    return true;
  }
  if (callee.type == Type::Fn && callee.obj) {
    std::shared_ptr<ClosureObj> owned;
    ClosureObj* closure = closure_of(callee, owned);
    if (!closure) { fail(line, "that function was never closed over"); return false; }
    if (argc != closure->fn->arity) {
      fail(line, closure->fn->name + "() takes " + std::to_string(closure->fn->arity) +
           " argument(s), got " + std::to_string((int)argc));
      return false;
    }
    if (S.frames.size() >= jit::kMaxFrames) { fail(line, "call depth exceeded (runaway recursion?)"); return false; }
    const size_t base = S.stack.size() - argc - 1;
    S.frames.emplace_back(closure, 0, base, std::move(owned));
    if (jit_enabled) {
      if (void* code = native_for(*closure->fn)) return run_native(code, base);
    }
    return true;
  }
  if (callee.type == Type::BoundMethod && callee.obj) {
    auto* bm = static_cast<BoundMethodObj*>(callee.obj.get());
    if (argc < bm->min_args || (bm->max_args >= 0 && argc > bm->max_args)) {
      fail(line, bm->name + "() takes " + std::to_string(bm->min_args) +
           (bm->max_args == bm->min_args ? "" : "-" + (bm->max_args < 0 ? std::string("any") : std::to_string(bm->max_args))) +
           " argument(s), got " + std::to_string((int)argc));
      return false;
    }
    std::vector<Value> args(S.stack.end() - argc, S.stack.end());
    std::string err;
    Value out = bm->fn(*this, *bm, args, err);
    S.stack.resize(S.stack.size() - argc - 1);
    if (!err.empty()) { fail(line, err); return false; }
    S.stack.push_back(std::move(out));
    return true;
  }
  fail(line, std::string("cannot call a ") + type_name(callee.type) +
       " -- only a function or a native is callable");
  return false;
}

// The native code's Call and Return (grain_jit_arm64.cpp): the interpreter's
// own semantics without an interpreter entry per call.
bool Vm::jit_call(size_t ip) {
  PROF_COUNT(++g_jit_profile.slow_calls);
  State& S = *st_;
  Frame& f = S.frames.back();
  f.ip = ip + 2;                                   // where the interpreter leaves it: traces name this call
  const Chunk& chunk = *f.chunk;
  const uint8_t argc = chunk.code[ip + 1];
  // By reference: invoke() reads the callee before anything can grow the
  // stack, and the slot keeps it alive until the frame is torn down. (A copy
  // was an atomic refcount increment and decrement per call.)
  const int line = (ip < chunk.lines.size()) ? chunk.lines[ip] : 0;
  const int col = (ip < chunk.columns.size()) ? chunk.columns[ip] : 0;
  return invoke(S.stack[S.stack.size() - argc - 1], argc, line, col) && S.error.empty();
}

bool Vm::jit_get_global(size_t ip) {
  PROF_BEGIN();
  State& S = *st_;
  const Chunk& chunk = *S.frames.back().chunk;
  const uint16_t k = (uint16_t)((chunk.code[ip + 1] << 8) | chunk.code[ip + 2]);
  Value* g = cached_global(chunk, k, S.serial, S.globals);
  if (!g) {
    S.frames.back().ip = ip + 3;
    S.error = "'" + chunk.constants[k].str() + "' is not defined";
    S.error_line = (ip < chunk.lines.size()) ? chunk.lines[ip] : 0;
    S.error_column = (ip < chunk.columns.size()) ? chunk.columns[ip] : 0;
    return false;
  }
  push_copy(S.stack, *g);
  PROF_END(get_global);
  return true;
}

void Vm::jit_return() {
  PROF_BEGIN();
  State& S = *st_;
  const size_t base = S.frames.back().base;
  Value out = std::move(S.stack.back());
  S.stack.pop_back();
  close_upvalues_from(base);
  S.stack.resize(base);
  S.frames.pop_back();
  S.stack.push_back(std::move(out));
  PROF_END(ret);
}

bool Vm::execute_loop(size_t target_frame_depth, bool single_step) {
  State& S = *st_;

  auto frame_line = [&](const Frame& f) {
    const Chunk& c = f.closure->fn->chunk;
    const size_t at = f.ip > 0 ? f.ip - 1 : 0;
    return at < c.lines.size() ? c.lines[at] : 0;
  };

  auto frame_col = [&](const Frame& f) {
    const Chunk& c = f.closure->fn->chunk;
    const size_t at = f.ip > 0 ? f.ip - 1 : 0;
    return at < c.columns.size() ? c.columns[at] : 0;
  };

  if (S.frames.size() <= target_frame_depth) return S.error.empty();
  // The current frame and its chunk change only on Call and Return, so they
  // are re-read there instead of on every instruction. (Call includes natives,
  // which can re-enter the VM and reallocate `frames` - hence the reload after
  // any Call, not just grain-to-grain ones.)
  Frame* frame = &S.frames.back();
  const Chunk* cur = frame->chunk;
  prof_start();
  for (;;) {
    const Chunk& chunk = *cur;
    // Only a fault reads the line/col; looking it up per instruction was a load
    // on every step for nothing.
    struct LazyLine {
      const Chunk& c; size_t at;
      operator int() const { return at < c.lines.size() ? c.lines[at] : 0; }
    } line{chunk, frame->ip};
    struct LazyCol {
      const Chunk& c; size_t at;
      operator int() const { return at < c.columns.size() ? c.columns[at] : 0; }
    } col{chunk, frame->ip};

    auto fail = [&](int line_num, const std::string& msg, int col_num = -1) {
      S.error = msg;
      S.error_line = line_num;
      S.error_column = (col_num >= 0) ? col_num : (int)col;
    };

    if (g_prof_on) { g_prof_chunk = cur; g_prof_ip = frame->ip; }
    if (step_limit && ++steps > step_limit) {
      fail(frame_line(S.frames.back()),
           "step limit of " + std::to_string(step_limit) + " reached",
           frame_col(S.frames.back()));
      return false;
    }

    if (frame->ip >= chunk.code.size()) { fail(0, "ran off the end of a function", 0); break; }

    if (debug_state.enabled) {
      int cur_line = (frame->ip < chunk.lines.size()) ? chunk.lines[frame->ip] : 0;
      auto& seen = debug_state.frame_line;
      seen.resize(S.frames.size(), 0);                      // a frame that just began has seen no line yet
      if (cur_line > 0 && cur_line != seen.back()) {
        seen.back() = cur_line;
        const bool hit_bp = debug_state.breakpoints.count(cur_line) > 0;
        const bool hit_step = debug_state.step_line;
        const bool hit_step_over = debug_state.step_over && (S.frames.size() <= debug_state.step_over_depth);
        if (hit_bp || hit_step || hit_step_over) {
          debug_state.step_line = false;
          debug_state.step_over = false;
          if (debug_state.on_break) {
            std::string cur_origin = (frame->closure && frame->closure->fn) ? frame->closure->fn->name : "";
            debug_state.on_break(*this, cur_origin, cur_line);
            frame = &S.frames.back();                       // the hook may have run code
          }
        }
      }
    }

    const Op op = (Op)chunk.code[frame->ip++];

    auto read_byte = [&]() -> uint8_t { return chunk.code[frame->ip++]; };
    auto read_short = [&]() -> uint16_t {
      const uint16_t v = (uint16_t)((chunk.code[frame->ip] << 8) | chunk.code[frame->ip + 1]);
      frame->ip += 2;
      return v;
    };
    auto pop = [&]() { Value v = std::move(S.stack.back()); S.stack.pop_back(); return v; };

    switch (op) {
      case Op::Constant: push_copy(S.stack, chunk.constants[read_short()]); break;
      case Op::Nil: S.stack.push_back(Value::nil()); break;
      case Op::True: S.stack.push_back(Value::boolean(true)); break;
      case Op::False: S.stack.push_back(Value::boolean(false)); break;
      case Op::Pop: S.stack.pop_back(); break;

      case Op::GetLocal: push_copy(S.stack, S.stack[frame->base + read_byte()]); break;
      case Op::SetLocal: {
        Value& dst = S.stack[frame->base + read_byte()];
        const Value& top = S.stack.back();
        if (!dst.obj && !top.obj) { dst.type = top.type; dst.i = top.i; } else dst = top;
        break;
      }

      case Op::GetGlobal: {
        const uint16_t k = read_short();
        Value* g = cached_global(chunk, k, S.serial, S.globals);
        if (!g) { fail(line, "'" + chunk.constants[k].str() + "' is not defined"); break; }
        S.stack.push_back(*g);
        break;
      }
      case Op::SetGlobal: {
        const uint16_t k = read_short();
        Value* g = cached_global(chunk, k, S.serial, S.globals);
        // Assigning to a name that was never declared is a typo far more often
        // than an intent, and silently creating it is how one survives to
        // production.
        if (!g) { fail(line, "'" + chunk.constants[k].str() + "' is not defined"); break; }
        *g = S.stack.back();
        break;
      }
      case Op::DefineGlobal: {
        const std::string& name = chunk.constants[read_short()].str();
        S.globals[name] = pop();
        break;
      }

      case Op::GetUpvalue: {
        const auto& uv = frame->closure->upvalues[read_byte()];
        S.stack.push_back(uv->is_closed ? uv->closed : S.stack[uv->slot]);
        break;
      }
      case Op::SetUpvalue: {
        const auto& uv = frame->closure->upvalues[read_byte()];
        if (uv->is_closed) uv->closed = S.stack.back();
        else S.stack[uv->slot] = S.stack.back();
        break;
      }

      case Op::Equal: case Op::NotEqual: {
        Value& a = S.stack[S.stack.size() - 2];
        const Value& b = S.stack.back();
        if (both_int(a, b)) {           // in place: no Value built or destroyed
          const bool eq = a.i == b.i;
          a.type = Type::Bool; a.i = ((op == Op::Equal) ? eq : !eq) ? 1 : 0;   // whole word, as Value::boolean leaves it
          S.stack.pop_back();
          break;
        }
        Value bv = pop(), av = pop();
        S.stack.push_back(Value::boolean((op == Op::Equal) == av.equals(bv)));
        break;
      }

      case Op::Less: case Op::LessEqual: case Op::Greater: case Op::GreaterEqual: {
        {
          Value& a = S.stack[S.stack.size() - 2];
          const Value& b = S.stack.back();
          if (both_int(a, b)) {
            const bool r = op == Op::Less ? a.i < b.i : op == Op::LessEqual ? a.i <= b.i
                         : op == Op::Greater ? a.i > b.i : a.i >= b.i;
            a.type = Type::Bool; a.i = r ? 1 : 0;
            S.stack.pop_back();
            break;
          }
        }
        Value b = pop(), a = pop();
        int cmp = 0;
        if (decimal_pair(a, b)) {
          cmp = decimal_compare(a, b);
        } else if (numeric_pair(a, b)) {
          if (both_int(a, b)) cmp = a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
          else { const double x = a.as_double(), y = b.as_double(); cmp = x < y ? -1 : (x > y ? 1 : 0); }
        } else if (a.type == Type::Str && b.type == Type::Str) {
          const int r = a.str().compare(b.str());
          cmp = r < 0 ? -1 : (r > 0 ? 1 : 0);
        } else if (a.type == Type::Id128 && b.type == Type::Id128) {
          const auto& x = a.as_id128();
          const auto& y = b.as_id128();
          cmp = x < y ? -1 : (y < x ? 1 : 0);
        } else {
          fail(line, std::string("cannot compare ") + type_name(a.type) + " with " + type_name(b.type));
          break;
        }
        bool r = false;
        if (op == Op::Less) r = cmp < 0;
        else if (op == Op::LessEqual) r = cmp <= 0;
        else if (op == Op::Greater) r = cmp > 0;
        else r = cmp >= 0;
        S.stack.push_back(Value::boolean(r));
        break;
      }

      case Op::Add: {
        {
          Value& x = S.stack[S.stack.size() - 2];
          const Value& y = S.stack.back();
          if (both_int(x, y)) { x.i += y.i; S.stack.pop_back(); break; }
          if (x.type == Type::Real && y.type == Type::Real) { x.d += y.d; S.stack.pop_back(); break; }
        }
        Value b = pop(), a = pop();
        if (both_int(a, b)) { S.stack.push_back(Value::integer(a.i + b.i)); break; }
        if (numeric_pair(a, b)) { S.stack.push_back(Value::real(a.as_double() + b.as_double())); break; }
        if (a.type == Type::Decimal || b.type == Type::Decimal) {
          Value r;
          std::string why;
          if (!decimal_arith('+', a, b, r, why)) { fail(line, why); break; }
          S.stack.push_back(std::move(r));
          break;
        }
        if (a.type == Type::Str && b.type == Type::Str) { S.stack.push_back(Value::string(a.str() + b.str())); break; }
        if (a.type == Type::Bytes && b.type == Type::Bytes) {
          std::vector<uint8_t> out = a.bytes_data();
          out.insert(out.end(), b.bytes_data().begin(), b.bytes_data().end());
          S.stack.push_back(Value::bytes(std::move(out)));
          break;
        }
        if (a.type == Type::List && b.type == Type::List) {
          std::vector<Value> out = a.list_items();
          const auto& more = b.list_items();
          out.insert(out.end(), more.begin(), more.end());
          S.stack.push_back(Value::list(std::move(out)));
          bool has_c = false;
          for (const auto& item : S.stack.back().list_items()) if (item.is_container()) { has_c = true; break; }
          if (has_c) track_container(S.stack.back().obj);
          break;
        }
        // No implicit "a" + 1. The one time it is what you meant it saves four
        // characters; every other time it hides that a number arrived where a
        // string was expected.
        fail(line, std::string("cannot add ") + type_name(a.type) + " and " + type_name(b.type));
        break;
      }
      case Op::Subtract: case Op::Multiply: {
        {
          Value& x = S.stack[S.stack.size() - 2];
          const Value& y = S.stack.back();
          if (both_int(x, y)) { x.i = op == Op::Subtract ? x.i - y.i : x.i * y.i; S.stack.pop_back(); break; }
          if (x.type == Type::Real && y.type == Type::Real) { x.d = op == Op::Subtract ? x.d - y.d : x.d * y.d; S.stack.pop_back(); break; }
        }
        Value b = pop(), a = pop();
        if (a.type == Type::Decimal || b.type == Type::Decimal) {
          Value r;
          std::string why;
          if (!decimal_arith(op == Op::Subtract ? '-' : '*', a, b, r, why)) { fail(line, why); break; }
          S.stack.push_back(std::move(r));
          break;
        }
        if (!numeric_pair(a, b)) {
          fail(line, std::string("cannot ") + (op == Op::Subtract ? "subtract " : "multiply ") +
               type_name(a.type) + " and " + type_name(b.type));
          break;
        }
        if (both_int(a, b)) {
          S.stack.push_back(Value::integer(op == Op::Subtract ? a.i - b.i : a.i * b.i));
        } else {
          const double x = a.as_double(), y = b.as_double();
          S.stack.push_back(Value::real(op == Op::Subtract ? x - y : x * y));
        }
        break;
      }
      // Division always produces a real. Integer division that silently
      // truncates is the single most reliable source of an off-by-one in any
      // language that has it; `int(a / b)` says what it means.
      case Op::Divide: {
        Value b = pop(), a = pop();
        if (a.type == Type::Decimal || b.type == Type::Decimal) {
          Value r;
          std::string why;
          if (!decimal_arith('/', a, b, r, why)) { fail(line, why); break; }
          S.stack.push_back(std::move(r));
          break;
        }
        if (!numeric_pair(a, b)) {
          fail(line, std::string("cannot divide ") + type_name(a.type) + " by " + type_name(b.type));
          break;
        }
        if (b.as_double() == 0.0) { fail(line, "division by zero"); break; }
        S.stack.push_back(Value::real(a.as_double() / b.as_double()));
        break;
      }
      case Op::Modulo: {
        Value b = pop(), a = pop();
        if (a.type == Type::Decimal || b.type == Type::Decimal) {
          Value r;
          std::string why;
          if (!decimal_arith('%', a, b, r, why)) { fail(line, why); break; }
          S.stack.push_back(std::move(r));
          break;
        }
        if (!numeric_pair(a, b)) {
          fail(line, std::string("cannot take ") + type_name(a.type) + " modulo " + type_name(b.type));
          break;
        }
        if (both_int(a, b)) {
          if (b.i == 0) { fail(line, "modulo by zero"); break; }
          // INT64_MIN % -1 traps on x86 (SIGFPE); the answer is 0 everywhere.
          S.stack.push_back(Value::integer(b.i == -1 ? 0 : a.i % b.i));
        } else {
          if (b.as_double() == 0.0) { fail(line, "modulo by zero"); break; }
          S.stack.push_back(Value::real(std::fmod(a.as_double(), b.as_double())));
        }
        break;
      }
      // Bits are an integer's: 1.5 | 0 is an error, not a silent truncation
      // (the same rule that keeps "a" + 1 an error). Shifts: a negative count
      // is an error; 64 or more shifts everything out - 0 to the left, the
      // sign to the right (-8 >> 99 is -1, as -8 >> 63 is).
      case Op::BitAnd: case Op::BitOr: case Op::BitXor: case Op::Shl: case Op::Shr: {
        Value& x = S.stack[S.stack.size() - 2];
        const Value& y = S.stack.back();
        if (!both_int(x, y)) {
          fail(line, std::string("bitwise operations take integers, got ") + type_name(x.type) + " and " + type_name(y.type));
          break;
        }
        if ((op == Op::Shl || op == Op::Shr) && y.i < 0) { fail(line, "negative shift count"); break; }
        if (op == Op::BitAnd) x.i = x.i & y.i;
        else if (op == Op::BitOr) x.i = x.i | y.i;
        else if (op == Op::BitXor) x.i = x.i ^ y.i;
        else if (op == Op::Shl) x.i = y.i < 64 ? (int64_t)((uint64_t)x.i << y.i) : 0;
        else x.i = y.i < 64 ? (x.i >> y.i) : (x.i < 0 ? -1 : 0);
        S.stack.pop_back();
        break;
      }
      case Op::BitNot: {
        Value& a = S.stack.back();
        if (a.type != Type::Int) { fail(line, std::string("cannot bitwise invert a ") + type_name(a.type)); break; }
        a.i = ~a.i;
        break;
      }
      case Op::Dup: push_copy(S.stack, S.stack.back()); break;
      case Op::Dup2: {
        push_copy(S.stack, S.stack[S.stack.size() - 2]);
        push_copy(S.stack, S.stack[S.stack.size() - 2]);
        break;
      }
      case Op::Negate: {
        Value a = pop();
        if (a.type == Type::Int) S.stack.push_back(Value::integer(-a.i));
        else if (a.type == Type::Real) S.stack.push_back(Value::real(-a.d));
        else if (a.type == Type::Decimal) { Value r; std::string why; decimal_negate(a, r, why); S.stack.push_back(std::move(r)); }
        else { fail(line, std::string("cannot negate a ") + type_name(a.type)); }
        break;
      }
      case Op::Not: { Value a = pop(); S.stack.push_back(Value::boolean(!a.truthy())); break; }

      case Op::Jump: frame->ip += read_short(); break;
      case Op::JumpIfFalse: { const uint16_t d = read_short(); if (!S.stack.back().truthy()) frame->ip += d; break; }
      case Op::JumpIfTrue: { const uint16_t d = read_short(); if (S.stack.back().truthy()) frame->ip += d; break; }
      case Op::Loop: { const uint16_t d = read_short(); frame->ip -= d; break; }

      case Op::Call: {
        const uint8_t argc = read_byte();
        const bool ok = invoke(S.stack[S.stack.size() - argc - 1], argc, line, col);
        frame = &S.frames.back(); cur = frame->chunk;
        if (!ok) break;
        break;
      }

      case Op::Closure: {
        auto fn = std::static_pointer_cast<FnObj>(chunk.constants[read_short()].obj);
        auto closure = std::make_shared<ClosureObj>();
        closure->fn = fn;
        for (size_t u = 0; u < fn->upvalue_index.size(); ++u) {
          const bool from_local = chunk.code[frame->ip++] != 0;
          const uint8_t index = chunk.code[frame->ip++];
          if (from_local) {
            const size_t slot = frame->base + index;
            // One Upvalue per slot, shared. Two closures capturing the same
            // variable have to SEE each other's writes -- a counter handed to
            // an incrementer and a reader is the whole point.
            std::shared_ptr<Upvalue> found;
            for (auto& o : S.open) if (!o->is_closed && o->slot == slot) { found = o; break; }
            if (!found) {
              found = std::make_shared<Upvalue>();
              found->slot = slot;
              S.open.push_back(found);
            }
            closure->upvalues.push_back(found);
          } else {
            closure->upvalues.push_back(frame->closure->upvalues[index]);
          }
        }
        S.stack.push_back(Value::object(Type::Fn, closure));
        track_container(closure);
        break;
      }

      case Op::CloseUpvalue:
        close_upvalues_from(S.stack.size() - 1);
        S.stack.pop_back();
        break;

      case Op::Return: {
        Value out = pop();
        close_upvalues_from(frame->base);
        S.stack.resize(frame->base);
        S.frames.pop_back();
        S.stack.push_back(std::move(out));
        if (S.frames.size() <= target_frame_depth) {
          return true;
        }
        frame = &S.frames.back(); cur = frame->chunk;
        break;
      }

      case Op::BuildList: {
        const uint16_t n = read_short();
        std::vector<Value> items(S.stack.end() - n, S.stack.end());
        S.stack.resize(S.stack.size() - n);
        bool has_c = false;
        for (const auto& it : items) if (it.is_container()) { has_c = true; break; }
        S.stack.push_back(Value::list(std::move(items)));
        if (has_c) track_container(S.stack.back().obj);
        break;
      }
      case Op::ListConcat: {
        const uint16_t n = read_short();
        const size_t first = S.stack.size() - n;
        size_t total = 0;
        bool bad = false;
        for (size_t k = first; k < S.stack.size(); ++k) {
          if (S.stack[k].type != Type::List) {
            fail(line, std::string("... spreads a list, not a ") + type_name(S.stack[k].type));
            bad = true;
            break;
          }
          total += S.stack[k].list_items().size();
        }
        if (bad) break;
        std::vector<Value> items;
        items.reserve(total);
        bool has_c = false;
        for (size_t k = first; k < S.stack.size(); ++k)
          for (const auto& it : S.stack[k].list_items()) {
            if (it.is_container()) has_c = true;
            items.push_back(it);
          }
        S.stack.resize(first);
        S.stack.push_back(Value::list(std::move(items)));
        if (has_c) track_container(S.stack.back().obj);
        break;
      }
      case Op::CallList: {
        // [callee, list] -> [callee, items...], then the call as Call makes it
        Value args = pop();
        const auto& items = args.list_items();
        if (items.size() > 255) { fail(line, "more than 255 arguments"); break; }
        for (const auto& it : items) S.stack.push_back(it);
        const uint8_t argc = (uint8_t)items.size();
        const bool ok = invoke(S.stack[S.stack.size() - argc - 1], argc, line, col);
        frame = &S.frames.back(); cur = frame->chunk;
        if (!ok) break;
        break;
      }
      case Op::BuildMap: {
        const uint16_t n = read_short();
        auto m = std::make_shared<MapObj>();
        m->entries.reserve(n);
        bool has_c = false;
        for (size_t i = 0; i < n; ++i) {
          const Value& k = S.stack[S.stack.size() - 2 * (n - i)];
          const Value& v = S.stack[S.stack.size() - 2 * (n - i) + 1];
          if (v.is_container()) has_c = true;
          m->put(k.str(), v);
        }
        S.stack.resize(S.stack.size() - 2 * (size_t)n);
        S.stack.push_back(Value::object(Type::Map, m));
        if (has_c) track_container(m);
        break;
      }

      case Op::IndexGet: {
        Value key = pop(), target = pop();
        if (target.type == Type::List) {
          if (!key.is_number()) { fail(line, "a list index must be a number"); break; }
          auto& items = target.list_items();
          int64_t at = key.as_int();
          if (at < 0) at += (int64_t)items.size();   // a[-1] is the last element
          if (at < 0 || at >= (int64_t)items.size()) {
            fail(line, "index " + std::to_string(key.as_int()) + " is outside a list of " +
                 std::to_string(items.size()));
            break;
          }
          S.stack.push_back(items[(size_t)at]);
          break;
        }
        if (target.type == Type::Map) {
          if (key.type != Type::Str) { fail(line, "a map key must be a string"); break; }
          Value* v = static_cast<MapObj*>(target.obj.get())->find(key.str());
          S.stack.push_back(v ? *v : Value::nil());
          break;
        }
        if (target.type == Type::Bytes) {
          if (!key.is_number()) { fail(line, "a bytes index must be a number"); break; }
          const auto& d = target.bytes_data();
          int64_t at = key.as_int();
          if (at < 0) at += (int64_t)d.size();
          if (at < 0 || at >= (int64_t)d.size()) {
            fail(line, "index " + std::to_string(key.as_int()) + " is outside bytes of " + std::to_string(d.size()));
            break;
          }
          S.stack.push_back(Value::integer(d[(size_t)at]));
          break;
        }
        if (target.type == Type::Str) {
          if (!key.is_number()) { fail(line, "a string index must be a number"); break; }
          const std::string& s = target.str();
          int64_t at = key.as_int();
          if (at < 0) at += (int64_t)s.size();
          if (at < 0 || at >= (int64_t)s.size()) { fail(line, "string index out of range"); break; }
          S.stack.push_back(Value::string(std::string(1, s[(size_t)at])));
          break;
        }
        fail(line, std::string("cannot index a ") + type_name(target.type));
        break;
      }

      case Op::IndexSet: {
        Value v = pop(), key = pop(), target = pop();
        if (v.is_container() && target.obj) track_container(target.obj);
        if (target.type == Type::List) {
          if (!key.is_number()) { fail(line, "a list index must be a number"); break; }
          auto& items = target.list_items();
          int64_t at = key.as_int();
          if (at < 0) at += (int64_t)items.size();
          if (at < 0 || at >= (int64_t)items.size()) { fail(line, "list index out of range"); break; }
          items[(size_t)at] = v;
        } else if (target.type == Type::Bytes) {
          if (!key.is_number()) { fail(line, "a bytes index must be a number"); break; }
          auto& d = target.bytes_data();
          int64_t at = key.as_int();
          if (at < 0) at += (int64_t)d.size();
          if (at < 0 || at >= (int64_t)d.size()) { fail(line, "bytes index out of range"); break; }
          if (v.type != Type::Int || v.i < 0 || v.i > 255) { fail(line, "a byte is a whole number from 0 to 255"); break; }
          d[(size_t)at] = (uint8_t)v.i;
        } else if (target.type == Type::Map) {
          if (key.type != Type::Str) { fail(line, "a map key must be a string"); break; }
          static_cast<MapObj*>(target.obj.get())->put(key.str(), v);
        } else {
          fail(line, std::string("cannot assign into a ") + type_name(target.type));
          break;
        }
        S.stack.push_back(v);
        break;
      }

            case Op::FieldGet: {
        const uint16_t const_k = read_short();
        const std::string& name = chunk.constants[const_k].str();
        Value target = pop();
        auto resolve_field = [&](const Value& tgt, const std::string& fn, Value& out) -> bool {
          if (tgt.type == Type::Map && tgt.obj) {
            Value* v = static_cast<MapObj*>(tgt.obj.get())->find(fn);
            out = v ? *v : Value::nil();
            return true;
          }
          if (tgt.type == Type::Id128) {
            const auto& id = tgt.as_id128();
            if (fn == "hi") { out = Value::integer((int64_t)id.hi); return true; }
            if (fn == "lo") { out = Value::integer((int64_t)id.lo); return true; }
            if (fn == "str" || fn == "hex") { out = Value::string(id.str()); return true; }
            if (fn == "short") { out = Value::string(id.short_str()); return true; }
          }
          if (tgt.type == Type::Capability && tgt.obj) {
            auto* cap = static_cast<CapObj*>(tgt.obj.get());
            if (fn == "id") { out = Value::id128(cap->id); return true; }
            if (fn == "resource") { out = Value::string(cap->resource); return true; }
            if (fn == "rights") { out = Value::integer((int64_t)cap->rights); return true; }
            if (fn == "expiry" || fn == "expires") { out = Value::integer((int64_t)cap->expiry); return true; }
            if (fn == "parent") { out = Value::id128(cap->parent_id); return true; }
            if (fn == "holder") { out = Value::string(cap->holder); return true; }
            if (fn == "attenuate") {
              out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(tgt, "attenuate", 1, 1, method_cap_attenuate));
              return true;
            }
            if (fn == "can") {
              out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(tgt, "can", 1, 1, method_cap_can));
              return true;
            }
            if (fn == "has_expired") {
              out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(tgt, "has_expired", 0, 1, method_cap_has_expired));
              return true;
            }
          }
          if (tgt.type == Type::Channel && tgt.obj) {
            auto* ch = static_cast<ChannelObj*>(tgt.obj.get());
            if (fn == "id") { out = Value::id128(ch->id); return true; }
            if (fn == "name") { out = Value::string(ch->name); return true; }
            if (fn == "capacity") { out = Value::integer((int64_t)ch->capacity); return true; }
            if (fn == "generation") { out = Value::integer((int64_t)ch->generation); return true; }
            if (fn == "head") { out = Value::integer((int64_t)ch->head); return true; }
            if (fn == "tail") { out = Value::integer((int64_t)ch->tail); return true; }
            if (fn == "dropped") { out = Value::integer((int64_t)ch->dropped); return true; }
            if (fn == "publish") {
              out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(tgt, "publish", 1, 2, method_channel_publish));
              return true;
            }
            if (fn == "consume") {
              out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(tgt, "consume", 0, 0, method_channel_consume));
              return true;
            }
            if (fn == "stats") {
              out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(tgt, "stats", 0, 0, method_channel_stats));
              return true;
            }
          }
          if (tgt.type == Type::StateFrame && tgt.obj) {
            auto* f = static_cast<FrameObj*>(tgt.obj.get());
            if (fn == "id") { out = Value::id128(f->id); return true; }
            if (fn == "generation") { out = Value::integer((int64_t)f->generation); return true; }
            if (fn == "publisher") { out = Value::id128(f->publisher); return true; }
            if (fn == "payload") { out = f->payload; return true; }
            if (fn == "timestamp") { out = Value::integer((int64_t)f->timestamp); return true; }
          }
          if (tgt.type == Type::MemoryObject && tgt.obj) {
            auto* mem = static_cast<MemObj*>(tgt.obj.get());
            if (fn == "id") { out = Value::id128(mem->id); return true; }
            if (fn == "size") { out = Value::integer((int64_t)mem->size); return true; }
            if (fn == "provider") { out = Value::string(mem->provider); return true; }
            if (fn == "generation") { out = Value::integer((int64_t)mem->generation); return true; }
            if (fn == "write") {
              out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(tgt, "write", 2, 2, method_mem_write));
              return true;
            }
            if (fn == "read") {
              out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(tgt, "read", 2, 2, method_mem_read));
              return true;
            }
            if (fn == "migrate") {
              out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(tgt, "migrate", 1, 1, method_mem_migrate));
              return true;
            }
          }
          if (tgt.type == Type::Entity && tgt.obj) {
            auto* ent = static_cast<EntityObj*>(tgt.obj.get());
            if (fn == "id" || fn == "identity") { out = Value::id128(ent->id); return true; }
            if (fn == "name") { out = Value::string(ent->name); return true; }
            if (fn == "dispatch") {
              out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(tgt, "dispatch", 1, 2, method_entity_dispatch));
              return true;
            }
            if (ent->handlers.find(fn) != ent->handlers.end()) {
              out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(tgt, fn, 0, 1, method_entity_handler_call));
              return true;
            }
            out = ent->get_field(fn);
            return true;
          }
          return false;
        };

        Value val = Value::nil();
        if (resolve_field(target, name, val)) {
          S.stack.push_back(std::move(val));
          break;
        }
        fail(line, std::string("a ") + type_name(target.type) + " has no field '" + name + "'");
        break;
      }
      case Op::OptFieldGet: {
        const uint16_t const_k = read_short();
        const std::string& name = chunk.constants[const_k].str();
        Value target = pop();
        if (target.type == Type::Nil) {
          S.stack.push_back(Value::nil());
          break;
        }
        auto resolve_field = [&](const Value& tgt, const std::string& fn, Value& out) -> bool {
          if (tgt.type == Type::Map && tgt.obj) {
            Value* v = static_cast<MapObj*>(tgt.obj.get())->find(fn);
            out = v ? *v : Value::nil();
            return true;
          }
          if (tgt.type == Type::Id128) {
            const auto& id = tgt.as_id128();
            if (fn == "hi") { out = Value::integer((int64_t)id.hi); return true; }
            if (fn == "lo") { out = Value::integer((int64_t)id.lo); return true; }
            if (fn == "str" || fn == "hex") { out = Value::string(id.str()); return true; }
            if (fn == "short") { out = Value::string(id.short_str()); return true; }
          }
          if (tgt.type == Type::Capability && tgt.obj) {
            auto* cap = static_cast<CapObj*>(tgt.obj.get());
            if (fn == "id") { out = Value::id128(cap->id); return true; }
            if (fn == "resource") { out = Value::string(cap->resource); return true; }
            if (fn == "rights") { out = Value::integer((int64_t)cap->rights); return true; }
            if (fn == "expiry" || fn == "expires") { out = Value::integer((int64_t)cap->expiry); return true; }
            if (fn == "parent") { out = Value::id128(cap->parent_id); return true; }
            if (fn == "holder") { out = Value::string(cap->holder); return true; }
            if (fn == "attenuate") {
              out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(tgt, "attenuate", 1, 1, method_cap_attenuate));
              return true;
            }
            if (fn == "can") {
              out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(tgt, "can", 1, 1, method_cap_can));
              return true;
            }
            if (fn == "has_expired") {
              out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(tgt, "has_expired", 0, 1, method_cap_has_expired));
              return true;
            }
          }
          if (tgt.type == Type::Channel && tgt.obj) {
            auto* ch = static_cast<ChannelObj*>(tgt.obj.get());
            if (fn == "id") { out = Value::id128(ch->id); return true; }
            if (fn == "name") { out = Value::string(ch->name); return true; }
            if (fn == "capacity") { out = Value::integer((int64_t)ch->capacity); return true; }
            if (fn == "generation") { out = Value::integer((int64_t)ch->generation); return true; }
            if (fn == "head") { out = Value::integer((int64_t)ch->head); return true; }
            if (fn == "tail") { out = Value::integer((int64_t)ch->tail); return true; }
            if (fn == "dropped") { out = Value::integer((int64_t)ch->dropped); return true; }
            if (fn == "publish") {
              out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(tgt, "publish", 1, 2, method_channel_publish));
              return true;
            }
            if (fn == "consume") {
              out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(tgt, "consume", 0, 0, method_channel_consume));
              return true;
            }
            if (fn == "stats") {
              out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(tgt, "stats", 0, 0, method_channel_stats));
              return true;
            }
          }
          if (tgt.type == Type::StateFrame && tgt.obj) {
            auto* f = static_cast<FrameObj*>(tgt.obj.get());
            if (fn == "id") { out = Value::id128(f->id); return true; }
            if (fn == "generation") { out = Value::integer((int64_t)f->generation); return true; }
            if (fn == "publisher") { out = Value::id128(f->publisher); return true; }
            if (fn == "payload") { out = f->payload; return true; }
            if (fn == "timestamp") { out = Value::integer((int64_t)f->timestamp); return true; }
          }
          if (tgt.type == Type::MemoryObject && tgt.obj) {
            auto* mem = static_cast<MemObj*>(tgt.obj.get());
            if (fn == "id") { out = Value::id128(mem->id); return true; }
            if (fn == "size") { out = Value::integer((int64_t)mem->size); return true; }
            if (fn == "provider") { out = Value::string(mem->provider); return true; }
            if (fn == "generation") { out = Value::integer((int64_t)mem->generation); return true; }
            if (fn == "write") {
              out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(tgt, "write", 2, 2, method_mem_write));
              return true;
            }
            if (fn == "read") {
              out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(tgt, "read", 2, 2, method_mem_read));
              return true;
            }
            if (fn == "migrate") {
              out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(tgt, "migrate", 1, 1, method_mem_migrate));
              return true;
            }
          }
          if (tgt.type == Type::Entity && tgt.obj) {
            auto* ent = static_cast<EntityObj*>(tgt.obj.get());
            if (fn == "id" || fn == "identity") { out = Value::id128(ent->id); return true; }
            if (fn == "name") { out = Value::string(ent->name); return true; }
            if (fn == "dispatch") {
              out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(tgt, "dispatch", 1, 2, method_entity_dispatch));
              return true;
            }
            if (ent->handlers.find(fn) != ent->handlers.end()) {
              out = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(tgt, fn, 0, 1, method_entity_handler_call));
              return true;
            }
            out = ent->get_field(fn);
            return true;
          }
          return false;
        };

        Value val = Value::nil();
        if (resolve_field(target, name, val)) {
          S.stack.push_back(std::move(val));
        } else {
          S.stack.push_back(Value::nil());
        }
        break;
      }
      case Op::FieldSet: {
        const uint16_t const_k = read_short();
        const std::string& name = chunk.constants[const_k].str();
        Value v = pop(), target = pop();
        if (v.is_container() && target.obj) track_container(target.obj);
        if (target.type == Type::Map && target.obj) {
          static_cast<MapObj*>(target.obj.get())->put(name, v);
          S.stack.push_back(v);
          break;
        }
        if (target.type == Type::Entity && target.obj) {
          static_cast<EntityObj*>(target.obj.get())->set_field(name, v);
          S.stack.push_back(v);
          break;
        }
        fail(line, std::string("cannot set field on a ") + type_name(target.type));
        break;
      }

      // A for loop walks a list. Anything else that can be walked is turned
      // into one here, once, so the loop instruction itself stays trivial.
      case Op::IterPrep: {
        Value seq = pop();
        if (seq.type == Type::List) { S.stack.push_back(seq); break; }
        if (seq.type == Type::Map) {
          std::vector<Value> keys;
          for (auto& [k, v] : static_cast<MapObj*>(seq.obj.get())->entries) keys.push_back(Value::string(k));
          S.stack.push_back(Value::list(std::move(keys)));
          break;
        }
        if (seq.type == Type::Bytes) {
          std::vector<Value> out;
          for (uint8_t byte : seq.bytes_data()) out.push_back(Value::integer(byte));
          S.stack.push_back(Value::list(std::move(out)));
          break;
        }
        if (seq.type == Type::Str) {
          std::vector<Value> chars;
          for (char ch : seq.str()) chars.push_back(Value::string(std::string(1, ch)));
          S.stack.push_back(Value::list(std::move(chars)));
          break;
        }
        fail(line, std::string("cannot walk a ") + type_name(seq.type));
        break;
      }

      case Op::IterNext: {
        const uint8_t seq_slot = read_byte();
        const Value& seq = S.stack[frame->base + seq_slot];
        Value& cursor = S.stack[frame->base + seq_slot + 1];
        const auto& items = seq.list_items();
        if ((size_t)cursor.i >= items.size()) { S.stack.push_back(Value::boolean(false)); break; }
        S.stack.push_back(items[(size_t)cursor.i]);
        cursor.i++;
        S.stack.push_back(Value::boolean(true));
        break;
      }

      case Op::Halt:
        return true;
    }

    if (!S.error.empty()) {
      return false;
    }
    if (single_step) return true;
  }

  return S.error.empty();
}

Vm::RunResult Vm::run(const std::shared_ptr<FnObj>& script) {
  RunResult result;
  if (!script) { result.error = "nothing to run"; return result; }

  State& S = *st_;
  S.stack.clear();
  S.frames.clear();
  S.open.clear();
  S.error.clear();
  steps = 0;

  auto top_closure = std::make_shared<ClosureObj>();
  top_closure->fn = script;
  S.stack.push_back(Value::object(Type::Fn, top_closure));
  S.frames.emplace_back(top_closure.get(), 0, 0);

  auto frame_line = [&](const Frame& f) {
    const Chunk& c = f.closure->fn->chunk;
    const size_t at = f.ip > 0 ? f.ip - 1 : 0;
    return at < c.lines.size() ? c.lines[at] : 0;
  };
  auto frame_col = [&](const Frame& f) {
    const Chunk& c = f.closure->fn->chunk;
    const size_t at = f.ip > 0 ? f.ip - 1 : 0;
    return at < c.columns.size() ? c.columns[at] : 0;
  };

  auto capture_trace = [&]() {
    const size_t n = S.frames.size();
    const size_t head = 6;
    for (size_t i = n; i-- > 0;) {
      const size_t shown = n - 1 - i;
      if (n > head + 2 && shown == head) {
        result.trace.push_back("... " + std::to_string(n - head - 1) + " more frame(s)");
        i = 1;   // the next iteration lands on the outermost frame
        continue;
      }
      const Frame& f = S.frames[i];
      int fl = frame_line(f);
      int fc = frame_col(f);
      std::string pos = "line " + std::to_string(fl);
      if (fc > 0) pos += ":" + std::to_string(fc);
      result.trace.push_back(f.closure->fn->name + " (" + pos + ")");
    }
  };

  bool ok;
  void* code = jit_enabled ? native_for(*script) : nullptr;
  if (code) ok = run_native(code, 0);
  else ok = execute_loop(0);
  if (ok && S.error.empty()) {
    result.ok = true;
    if (!S.stack.empty()) {
      result.value = S.stack.back();
    }
    return result;
  }

  capture_trace();
  result.error = S.error.empty() ? "stopped" : S.error;
  result.line = S.error_line;
  result.column = S.error_column;
  if (result.column == 0 && !S.frames.empty()) {
    result.column = frame_col(S.frames.back());
  }
  if (result.line == 0 && !S.frames.empty()) {
    result.line = frame_line(S.frames.back());
  }
  return result;
}

Value Vm::call(const Value& callee, const std::vector<Value>& args, std::string& error) {
  State& S = *st_;
  if (callee.type == Type::Native) {
    auto* nat = static_cast<NativeObj*>(callee.obj.get());
    if ((int)args.size() < nat->min_args || (nat->max_args >= 0 && (int)args.size() > nat->max_args)) {
      error = nat->name + "() takes " + std::to_string(nat->min_args) +
              (nat->max_args == nat->min_args ? "" : "-" + (nat->max_args < 0 ? std::string("any")
                                                           : std::to_string(nat->max_args))) +
              " argument(s), got " + std::to_string(args.size());
      return Value::nil();
    }
    std::vector<Value> arg_copy = args;
    return nat->fn(*this, arg_copy, error);
  }
  // A bound method (an FFI or native function, a plugin's, live()'s tick) is called as the interpreter calls it.
  if (callee.type == Type::BoundMethod && callee.obj) {
    auto* bm = static_cast<BoundMethodObj*>(callee.obj.get());
    if ((int)args.size() < bm->min_args || (bm->max_args >= 0 && (int)args.size() > bm->max_args)) {
      error = bm->name + "() takes " + std::to_string(bm->min_args) + " argument(s), got " + std::to_string(args.size());
      return Value::nil();
    }
    std::vector<Value> arg_copy = args;
    return bm->fn(*this, *bm, arg_copy, error);
  }

  std::shared_ptr<ClosureObj> owned;
  ClosureObj* closure = closure_of(callee, owned);
  if (!closure) {
    error = std::string("cannot call a ") + type_name(callee.type) +
            " -- only a function or a native is callable";
    return Value::nil();
  }

  if ((int)args.size() != closure->fn->arity) {
    error = closure->fn->name + "() takes " + std::to_string(closure->fn->arity) +
            " argument(s), got " + std::to_string(args.size());
    return Value::nil();
  }

  if (S.frames.size() >= jit::kMaxFrames) {
    error = "call depth exceeded (runaway recursion?)";
    return Value::nil();
  }

  size_t target_depth = S.frames.size();
  size_t base = S.stack.size();
  S.stack.push_back(callee);
  for (const auto& a : args) S.stack.push_back(a);
  S.frames.emplace_back(closure, 0, base, std::move(owned));

  std::string prev_error = S.error;
  int prev_error_line = S.error_line;
  int prev_error_column = S.error_column;
  S.error.clear();
  S.error_line = 0;
  S.error_column = 0;

  bool ok;
  void* code = jit_enabled ? native_for(*closure->fn) : nullptr;
  if (code) ok = run_native(code, base);
  else ok = execute_loop(target_depth);
  if (!ok || !S.error.empty()) {
    error = S.error;
    while (S.frames.size() > target_depth) S.frames.pop_back();
    S.stack.resize(base);
    S.error = prev_error;
    S.error_line = prev_error_line;
    S.error_column = prev_error_column;
    return Value::nil();
  }

  Value ret = Value::nil();
  if (!S.stack.empty()) {
    ret = S.stack.back();
    S.stack.pop_back();
  }
  S.error = prev_error;
  S.error_line = prev_error_line;
  S.error_column = prev_error_column;
  return ret;
}

}  // namespace aios::userland::grain
