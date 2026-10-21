#pragma once
// AQL — Query Language for the AIOS Object Store
// Object-store-native semantics (WHERE meta.*, NEAR, RELATED TO, HISTORY OF, AS OF)
#include <string>
#include <vector>
#include <memory>
#include "aios/core/result.hpp"
#include "aios/storage/object_store.hpp"
#include "aios/kernel/surface.hpp"

namespace aios::storage {

// Deterministic byte-trigram text embedding into 64 dimensions (L2-normalised).
std::vector<float> text_embedding(const std::string& text);

enum class AqlVerb {
  Select,
  Put
};

struct AqlPredicate {
  std::string field;  // "meta.kind", "meta.customer_id", "id", "timestamp", "derived_from", "provenance", "source_state"
  // "=", "!=", "LIKE", "CONTAINS", "<", "<=", ">", ">=". The ordering
  // comparisons are numeric when both sides parse as numbers and
  // lexicographic otherwise, so `meta.price > 100` means what it looks like
  // on a field the store holds as text.
  std::string op;
  std::string value;
  bool negated = false;   // NOT meta.status = "void"
};

struct AqlQuery {
  AqlVerb verb = AqlVerb::Select;

  // SELECT projection
  bool select_all = true;
  std::vector<std::string> select_fields;
  // SELECT COUNT(*) — the count of what matched, without carrying every
  // matched object back to the caller.
  bool count_only = false;

  // WHERE. `predicates` is one AND-list and stays the whole story for a query
  // built in C++ or written without OR. `predicate_groups` is the parsed form
  // in disjunctive normal form — an object matches if ANY group matches in
  // full — so `a = 1 AND b = 2 OR c = 3` means what a reader expects. When the
  // groups are empty the executor uses `predicates`.
  std::vector<AqlPredicate> predicates;
  std::vector<std::vector<AqlPredicate>> predicate_groups;

  // NEAR (Similarity search)
  bool has_near = false;
  std::vector<float> near_embedding;
  std::string near_text;

  // RELATED TO (Graph traversal)
  bool has_related = false;
  ObjectID related_id;
  bool has_related_kind = false;
  uint32_t related_kind = 0;
  std::string related_kind_str;

  // HISTORY OF (Derivation chain)
  bool has_history = false;
  ObjectID history_id;

  // AS OF (Checkpoint time projection)
  bool has_as_of = false;
  std::string as_of_checkpoint_node;
  uint64_t as_of_checkpoint_seq = 0;
  std::string as_of_path;
  uint64_t as_of_timestamp_ns = 0;

  // ORDER BY. Accepts "timestamp", "id", or any "meta.<key>" — a meta key
  // sorts numerically when the values are numbers. ORDER BY on a meta field
  // used to parse and then be ignored, so a query asking for the dearest
  // rows silently got the newest ones.
  std::string order_by = "timestamp";
  bool order_desc = true;
  // True when the query said ORDER BY. NEAR without one keeps its similarity
  // ranking; with one, the stated order wins.
  bool order_explicit = false;

  // LIMIT
  size_t limit = 0;

  // PUT / INSERT
  StoredObject put_object;
};

struct AqlQueryResult {
  bool success = true;
  std::string error;
  std::vector<StoredObject> objects;
  // The matched identities, without their contents. A COUNT is answered from
  // these, and so is a capability check — which needs to know WHICH object is
  // being read, not what is in it. Filled for count_only queries.
  std::vector<ObjectID> ids;
  size_t count = 0;
  std::string formatted;
};

// Parse AQL query string into AqlQuery structure
Result<AqlQuery> parse_aql(const std::string& query_str);

// Execute AQL query against ObjectStore directly (unscoped)
AqlQueryResult execute_aql(const ObjectStore& store, const AqlQuery& query, hal::Host* host = nullptr, const std::string& root_dir = "");

// Execute AQL query through Surface capability boundary (capability-scoped)
AqlQueryResult execute_aql(Surface& surface, const AqlQuery& query);

// Helper entrypoints
AqlQueryResult execute_aql(Surface& surface, const std::string& query_str);
AqlQueryResult execute_aql(const ObjectStore& store, const std::string& query_str, hal::Host* host = nullptr, const std::string& root_dir = "");

}  // namespace aios::storage
