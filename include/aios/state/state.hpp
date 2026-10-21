#pragma once
// State is a first-class kernel object, not a side effect of a process's RAM.
// A StateObject has an identity, a kind, a generation, an inline payload for
// small state and references to MemoryObjects for large state (tensors,
// activations, vectors). The kernel can capture and restore it.
#include <vector>
#include <functional>
#include "aios/hal/sync.hpp"
#include <unordered_map>
#include <unordered_set>
#include "aios/core/ids.hpp"
#include "aios/core/result.hpp"
#include "aios/core/bytes.hpp"

namespace aios {

enum class StateKind : uint8_t { Working = 0, Persistent = 1, Context = 2, Relationship = 3, Execution = 4 };
const char* state_kind_name(StateKind k);
bool parse_state_kind(const std::string& s, StateKind& out);

struct StateObject {
  StateID id;
  EntityID owner;
  StateKind kind = StateKind::Working;
  uint64_t generation = 0;
  uint64_t created_ns = 0;
  uint64_t updated_ns = 0;
  ByteBuffer payload;                 // small inline state, kernel-native bytes
  std::vector<MemoryID> memory;       // large state lives in memory objects
  uint64_t fingerprint() const;

  void encode(Writer& w) const;
  static Result<StateObject> decode(Reader& r);
};

class StateStore {
 public:
  explicit StateStore(IdGenerator& ids);
  // Told after every change: the state as it now is, or nullptr with its id
  // when it is gone. Called outside the store's lock. The kernel uses it to
  // keep residents' state in the root across boots.
  // Only DURABLE states are reported (see set_durable); the rest live and die
  // with the process, as they always have.
  using Persist = std::function<void(const StateID& id, const StateObject* now)>;
  void persist_with(Persist p) { persist_ = std::move(p); }
  // Mark a state as one that outlives the process; it is reported now and on
  // every later change. False when there is no such state.
  bool set_durable(const StateID& id, bool on = true);
  bool is_durable(const StateID& id) const;
  Result<StateID> create(const EntityID& owner, StateKind kind, uint64_t now_ns, ByteSpan payload = {});
  Result<StateID> install(const StateObject& s);     // restore path
  const StateObject* get(const StateID& id) const;
  Result<StateObject> copy(const StateID& id) const;
  Result<void> set_payload(const StateID& id, ByteSpan payload, uint64_t now_ns);
  Result<void> attach_memory(const StateID& id, const MemoryID& mem, uint64_t now_ns);
  Result<void> detach_memory(const StateID& id, const MemoryID& mem, uint64_t now_ns);
  Result<uint64_t> apply_delta(const StateID& id, const EntityID& owner, StateKind kind, uint64_t target_gen, ByteSpan payload, const std::vector<MemoryID>& mems, uint64_t now_ns);
  Result<void> destroy(const StateID& id);
  Result<void> destroy_owned(const EntityID& owner);
  std::vector<StateObject> list(const EntityID& owner = {}) const;
  size_t count() const;
 private:
  IdGenerator& ids_;
  mutable hal::Mutex mu_;
  std::unordered_map<StateID, StateObject, TypedIdHash<3>> states_;
  Persist persist_;
  std::unordered_set<StateID, TypedIdHash<3>> durable_;
  void changed(const StateID& id) const;   // takes the lock itself; call without it
};

}  // namespace aios
