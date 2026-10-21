#pragma once
// Standing directives: work this machine does because it was told to once, not
// because someone is typing.
//
// Everything in this operating system so far happens when a person asks for it.
// A directive is the other thing — a line of the machine's own command surface,
// an interval, and the entity whose authority it runs with. The kernel holds
// them as state, so they survive a checkpoint and come back with the machine;
// something that pumps the machine while it is idle runs whatever is due.
//
// The authority matters as much as the schedule. A directive does not run as
// "the system": it runs as an entity, and everything it does is checked against
// what that entity holds — which is how a standing order can be given to a
// machine without handing it the machine.
#include <cstdint>
#include <string>
#include <vector>

#include "aios/core/result.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"

namespace aios::userland {

struct Directive {
  std::string name;
  std::string line;          // a command-surface line, run by the machine on itself
  uint64_t every_ms = 0;     // 0 means once, when it is next pumped
  uint64_t due_ms = 0;       // wall-clock milliseconds since the Unix epoch
  uint64_t runs = 0;
  uint64_t last_ms = 0;
  uint64_t last_took_ms = 0;
  bool enabled = true;
  bool last_ok = true;
  std::string last_result;   // the first line of what it said, or why it failed
  EntityID as;               // the authority it acts with
};

class Directives {
 public:
  explicit Directives(Surface& actor);

  // Adopt the register this kernel already holds (including one that came back
  // in a checkpoint image), or create it, seeded from the durable copy the last
  // process left under the root.
  // `create_if_missing` is false for anyone who is only reading. It matters:
  // the idle pump asks this question thousands of times a second on the metal,
  // and a register minted per question is a state object per question.
  Result<void> attach(bool create_if_missing = true);
  Result<void> add(const Directive& d);
  Result<void> remove(const std::string& name);
  Result<void> set_enabled(const std::string& name, bool on);
  // What is due at `now`, in the order they were given.
  std::vector<Directive> due(uint64_t now_ms) const;
  // Record what happened, and when it is next due.
  Result<void> note_run(const std::string& name, uint64_t now_ms, uint64_t took_ms, bool ok,
                        const std::string& result);

  const std::vector<Directive>& all() const { return directives_; }
  const Directive* find(const std::string& name) const;
  const StateID& state() const { return state_; }
  bool adopted() const { return adopted_; }
  std::string register_path() const;
  // The entity a directive acts as has to outlive the process that gave the
  // order, or the order is worthless. Identity survives here the way it
  // survives everywhere else in this OS — by checkpoint and restore — so the
  // register keeps one, and brings it back when it attaches to a kernel that
  // does not have it.
  std::string authority_path(const EntityID& who) const;
  Result<size_t> restore_missing();

 private:
  Result<void> commit();
  ByteBuffer encode() const;
  void decode(ByteSpan b);

  Surface& me_;
  StateID state_;
  EntityID holder_;
  bool adopted_ = false;
  std::vector<Directive> directives_;
};

}  // namespace aios::userland
