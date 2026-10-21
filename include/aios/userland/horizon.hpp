#pragma once
// The horizon: what the world did that touches the work this machine holds.
//
// `sense` gives this kernel eyes. On its own that is a fetch — a page arrives,
// it goes in the object store, and nothing has been understood. The horizon is
// the other half: the machine decides what it CARES about from what it actually
// holds, reads its sources against that, and keeps only the few things that
// touch its own work. What survives is a signal, and the signals together are
// the map you walk up to the glass and read.
//
// Two things make it honest rather than a feed reader:
//
//  - Interest is DERIVED, never configured. It comes from the repositories this
//    kernel has registered and the subjects of the commits written into them —
//    the machine's own coding history, which is the only thing here that knows
//    what its operator is working on. Change what you work on and the horizon
//    moves, because nobody typed the terms.
//
//  - Nothing runs as "the machine". A scan reaches out and keeps what it found
//    as an entity, and both halves are checked separately: an entity that may
//    look but not keep can read the world and change nothing.
//
// A standing directive of `horizon scan` is what makes the whole thing
// autonomous — the register is kernel state, so the order and the authority it
// acts with both survive a checkpoint and come back with the machine.
#include <cstdint>
#include <string>
#include <vector>

#include "aios/core/result.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/userland/sense.hpp"

namespace aios::userland {

// A term this machine cares about, and where that came from. `weight` is how
// much a match on it counts.
struct Interest {
  std::string term;
  double weight = 1.0;
  std::string from;      // "repository", "commit subjects", …
  uint64_t seen = 0;     // how often the machine's own history says it
};

// One thing on the horizon: something the world published that matched.
struct Signal {
  std::string title;
  std::string url;
  std::string source;              // the source it came off
  double score = 0;
  std::vector<std::string> matched;  // the machine's own terms it touched
  uint64_t first_seen_ms = 0;
  ObjectID object;                 // what it became in the store
  bool fresh = false;              // kept by the scan that just ran
};

// A place the machine looks. Not a subscription: a page it reads the links off.
struct HorizonSource {
  std::string name;
  std::string url;
  uint64_t last_ms = 0;
  uint64_t reads = 0;
  uint64_t considered = 0;   // links the last read looked at
  uint64_t kept = 0;         // signals the last read kept
  bool last_ok = true;
  std::string last_result;
};

struct ScanReport {
  size_t sources_read = 0;
  size_t considered = 0;
  size_t kept = 0;
  size_t already_known = 0;
  uint64_t took_ms = 0;
  std::vector<Signal> signals;         // what this scan kept, best first
  std::vector<std::string> failures;   // "<source>: why"
};

class Horizon {
 public:
  explicit Horizon(Surface& actor);

  // Adopt the register this kernel holds (including one that came back in a
  // checkpoint image), or create it from the durable copy under the root.
  // `create_if_missing` is false for a reader — the canvas asks this question
  // every frame, and a register minted per question is a state object per frame.
  Result<void> attach(bool create_if_missing = true);

  Result<void> add_source(const std::string& name, const std::string& url);
  Result<void> remove_source(const std::string& name);
  const std::vector<HorizonSource>& sources() const { return sources_; }

  // What this machine cares about, worked out from what it holds right now.
  // Nothing is stored: ask again after a commit and the answer has moved.
  std::vector<Interest> interests() const;

  // Read every source, score what they point at against those interests, and
  // keep what clears the bar. Reaching out spends network authority and keeping
  // spends storage authority; the second failing does not undo the first.
  Result<ScanReport> scan(size_t keep_per_source = 6, double floor = 1.0);

  // Everything on the horizon now, best first — read back out of the store, so
  // it is whatever this machine knows rather than whatever this process did.
  std::vector<Signal> signals(size_t max = 40) const;

  const StateID& state() const { return state_; }
  bool adopted() const { return adopted_; }
  std::string register_path() const;

 private:
  Result<void> commit();
  ByteBuffer encode() const;
  void decode(ByteSpan b);

  Surface& me_;
  StateID state_;
  bool adopted_ = false;
  std::vector<HorizonSource> sources_;
};

// The scorer, on its own and testable on its own: how much a headline touches
// what this machine works on, and which of its terms it touched.
double horizon_score(const std::string& title, const std::vector<Interest>& interests,
                     std::vector<std::string>* matched);

// Words split out of a line of prose, lowercased, with the words that mean
// nothing dropped. Used for both halves — what a commit says and what a
// headline says are read the same way, or the comparison is meaningless.
std::vector<std::string> horizon_terms(const std::string& text);

}  // namespace aios::userland
