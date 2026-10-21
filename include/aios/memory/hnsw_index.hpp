#pragma once
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"
#include "aios/core/ids.hpp"

namespace aios::memory {

// Not thread-safe, including search_knn: a search stamps the shared visited
// array. The one owner (SemanticIndex) is only reached under the store's lock.
class HNSWVectorIndex {
 public:
  HNSWVectorIndex(size_t dimension = 128, size_t max_elements = 100000, size_t M = 16, size_t ef_construction = 200);

  Result<void> insert(const ObjectID& id, const std::vector<float>& vec);
  std::vector<std::pair<ObjectID, float>> search_knn(const std::vector<float>& query, size_t k = 5, size_t ef_search = 50) const;

  size_t size() const { return ids_.size(); }
  size_t dimension() const { return dimension_; }

 private:
  // Nodes are dense slots. Every neighbour visit used to be two hash lookups
  // by 128-bit id, and every search allocated a hash set to remember what it
  // had seen; slots make both an array index.
  using Slot = uint32_t;
  using Cand = std::pair<float, Slot>;

  size_t dimension_;
  size_t max_elements_;
  size_t M_;
  size_t ef_construction_;
  int max_layer_ = 0;
  Slot entry_ = 0;
  std::vector<ObjectID> ids_;
  std::vector<float> vectors_;                        // slot * dimension_, normalized
  std::vector<std::vector<std::vector<Slot>>> links_; // [slot][layer] -> neighbours
  std::unordered_map<ObjectID, Slot, Id128Hash> slot_of_;
  mutable std::vector<uint32_t> visited_;             // epoch stamp per slot
  mutable uint32_t epoch_ = 0;
  mutable std::vector<Cand> cand_heap_;
  mutable std::vector<Cand> best_heap_;
  // ef_search 16 was 4x faster but dropped the live store's recall@5 from
  // 97.5% to 94.2% (24 and 32: 95%); 50 keeps it and is still 15 us a query.

  const float* vec(Slot s) const { return vectors_.data() + (size_t)s * dimension_; }
  float distance(const float* q_norm, Slot s) const;
  void search_layer(const float* q_norm, Slot ep, float ep_dist, size_t ef, int layer, std::vector<Cand>& out) const;
  std::vector<Slot> select_neighbours(const std::vector<Cand>& nearest_first, size_t m) const;
  Slot greedy_descend(const float* q_norm, Slot ep, float& ep_dist, int from_layer, int to_layer) const;
};

}  // namespace aios::memory
