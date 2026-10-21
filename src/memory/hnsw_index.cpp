#include "aios/memory/hnsw_index.hpp"
#include <cmath>
#include <queue>
#include <algorithm>

#if defined(__AVX2__) && defined(__FMA__)
#include <immintrin.h>

struct QueryRegs {
  __m256 q[8];
};

inline float dot64_query(const QueryRegs& qr, const float* b) {
  __m256 s0 = _mm256_mul_ps(_mm256_loadu_ps(b), qr.q[0]);
  __m256 s1 = _mm256_mul_ps(_mm256_loadu_ps(b + 8), qr.q[1]);
  __m256 s2 = _mm256_mul_ps(_mm256_loadu_ps(b + 16), qr.q[2]);
  __m256 s3 = _mm256_mul_ps(_mm256_loadu_ps(b + 24), qr.q[3]);
  s0 = _mm256_fmadd_ps(_mm256_loadu_ps(b + 32), qr.q[4], s0);
  s1 = _mm256_fmadd_ps(_mm256_loadu_ps(b + 40), qr.q[5], s1);
  s2 = _mm256_fmadd_ps(_mm256_loadu_ps(b + 48), qr.q[6], s2);
  s3 = _mm256_fmadd_ps(_mm256_loadu_ps(b + 56), qr.q[7], s3);
  __m256 sum = _mm256_add_ps(_mm256_add_ps(s0, s1), _mm256_add_ps(s2, s3));
  __m128 lo = _mm256_castps256_ps128(sum);
  __m128 hi = _mm256_extractf128_ps(sum, 1);
  __m128 hsum = _mm_add_ps(lo, hi);
  hsum = _mm_add_ps(hsum, _mm_movehl_ps(hsum, hsum));
  hsum = _mm_add_ss(hsum, _mm_shuffle_ps(hsum, hsum, 1));
  return _mm_cvtss_f32(hsum);
}

inline float dot_general_avx2(const float* a, const float* b, size_t dim) {
  __m256 acc0 = _mm256_setzero_ps();
  __m256 acc1 = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 16 <= dim; i += 16) {
    acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), acc0);
    acc1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8), acc1);
  }
  for (; i + 8 <= dim; i += 8) {
    acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), acc0);
  }
  __m256 sum = _mm256_add_ps(acc0, acc1);
  __m128 lo = _mm256_castps256_ps128(sum);
  __m128 hi = _mm256_extractf128_ps(sum, 1);
  __m128 hsum = _mm_add_ps(lo, hi);
  hsum = _mm_add_ps(hsum, _mm_movehl_ps(hsum, hsum));
  hsum = _mm_add_ss(hsum, _mm_shuffle_ps(hsum, hsum, 1));
  float res = _mm_cvtss_f32(hsum);
  for (; i < dim; ++i) res += a[i] * b[i];
  return res;
}
#endif

namespace aios::memory {

HNSWVectorIndex::HNSWVectorIndex(size_t dimension, size_t max_elements, size_t M, size_t ef_construction)
    : dimension_(dimension), max_elements_(max_elements), M_(M), ef_construction_(ef_construction) {}

// Cosine distance in [0, 2] between unit-normalized vectors: 1.0f - dot.
// Vectors stored in vectors_ are unit-normalized upon insertion.
float HNSWVectorIndex::distance(const float* q_norm, Slot s) const {
  const float* b = vec(s);
#if defined(__AVX2__) && defined(__FMA__)
  if (dimension_ == 64) {
    QueryRegs qr;
    for (int i = 0; i < 8; ++i) qr.q[i] = _mm256_loadu_ps(q_norm + i * 8);
    float sim = dot64_query(qr, b);
    return 1.0f - sim;
  }
  float sim = dot_general_avx2(q_norm, b, dimension_);
  return 1.0f - sim;
#else
  float dot = 0.0f;
  for (size_t i = 0; i < dimension_; ++i) dot += q_norm[i] * b[i];
  return 1.0f - dot;
#endif
}

void HNSWVectorIndex::search_layer(const float* q_norm, Slot ep, float ep_dist,
                                   size_t ef, int layer, std::vector<Cand>& out) const {
  auto farther = [](const Cand& a, const Cand& b) { return a.first > b.first; };
  auto nearer = [](const Cand& a, const Cand& b) { return a.first < b.first; };

  cand_heap_.clear();
  best_heap_.clear();

  if (visited_.size() < ids_.size()) visited_.resize(ids_.size(), 0);
  if (++epoch_ == 0) { std::fill(visited_.begin(), visited_.end(), 0); epoch_ = 1; }
  visited_[ep] = epoch_;

  cand_heap_.push_back({ep_dist, ep});
  best_heap_.push_back({ep_dist, ep});

  float lower_bound = ep_dist;

#if defined(__AVX2__) && defined(__FMA__)
  const bool is_dim64 = (dimension_ == 64);
  QueryRegs qr;
  if (is_dim64) {
    for (int i = 0; i < 8; ++i) qr.q[i] = _mm256_loadu_ps(q_norm + i * 8);
  }
#endif

  while (!cand_heap_.empty()) {
    std::pop_heap(cand_heap_.begin(), cand_heap_.end(), farther);
    const Cand c = cand_heap_.back();
    cand_heap_.pop_back();

    if (c.first > lower_bound) break;

    if ((int)links_[c.second].size() <= layer) continue;
    const auto& nbrs = links_[c.second][layer];

    for (Slot nbr : nbrs) {
      if (visited_[nbr] == epoch_) continue;
      visited_[nbr] = epoch_;

      float d;
#if defined(__AVX2__) && defined(__FMA__)
      if (is_dim64) {
        d = 1.0f - dot64_query(qr, vec(nbr));
      } else {
        d = 1.0f - dot_general_avx2(q_norm, vec(nbr), dimension_);
      }
#else
      float dot = 0.0f;
      const float* b = vec(nbr);
      for (size_t i = 0; i < dimension_; ++i) dot += q_norm[i] * b[i];
      d = 1.0f - dot;
#endif

      if (best_heap_.size() < ef || d < lower_bound) {
        cand_heap_.push_back({d, nbr});
        std::push_heap(cand_heap_.begin(), cand_heap_.end(), farther);

        best_heap_.push_back({d, nbr});
        std::push_heap(best_heap_.begin(), best_heap_.end(), nearer);
        if (best_heap_.size() > ef) {
          std::pop_heap(best_heap_.begin(), best_heap_.end(), nearer);
          best_heap_.pop_back();
        }
        lower_bound = best_heap_.front().first;
      }
    }
  }

  out = best_heap_;
  std::sort(out.begin(), out.end());
}

std::vector<HNSWVectorIndex::Slot> HNSWVectorIndex::select_neighbours(const std::vector<Cand>& nearest_first,
                                                                      size_t m) const {
  std::vector<Slot> picked;
  for (const auto& [d, cand] : nearest_first) {
    if (picked.size() >= m) break;
    bool keep = true;
    for (Slot r : picked) {
      const float dr = distance(vec(cand), r);
      if (dr < d || dr <= 1e-6f) { keep = false; break; }
    }
    if (keep) picked.push_back(cand);
  }
  return picked;
}

HNSWVectorIndex::Slot HNSWVectorIndex::greedy_descend(const float* q_norm, Slot ep, float& ep_dist,
                                                      int from_layer, int to_layer) const {
#if defined(__AVX2__) && defined(__FMA__)
  const bool is_dim64 = (dimension_ == 64);
  QueryRegs qr;
  if (is_dim64) {
    for (int i = 0; i < 8; ++i) qr.q[i] = _mm256_loadu_ps(q_norm + i * 8);
  }
#endif

  for (int l = from_layer; l > to_layer; --l) {
    bool changed = true;
    while (changed) {
      changed = false;
      if ((int)links_[ep].size() <= l) break;
      for (Slot nbr : links_[ep][l]) {
        float d;
#if defined(__AVX2__) && defined(__FMA__)
        if (is_dim64) {
          d = 1.0f - dot64_query(qr, vec(nbr));
        } else {
          d = 1.0f - dot_general_avx2(q_norm, vec(nbr), dimension_);
        }
#else
        d = distance(q_norm, nbr);
#endif
        if (d < ep_dist) { ep_dist = d; ep = nbr; changed = true; }
      }
    }
  }
  return ep;
}

Result<void> HNSWVectorIndex::insert(const ObjectID& id, const std::vector<float>& v) {
  if (v.size() != dimension_) {
    return Result<void>(Status::Invalid, "Vector dimension mismatch");
  }

  uint64_t h = fnv1a(ByteSpan(reinterpret_cast<const uint8_t*>(&id), sizeof(id)));
  float r = (float)((h & 0xFFFFFF) + 1) / 16777216.0f;
  int target_layer = 0;
  double mL = 1.0 / std::log(double(M_));
  if (r > 0.0f) {
    target_layer = static_cast<int>(-std::log(r) * mL);
  }
  if (target_layer > 4) target_layer = 4;

  double sum_sq = 0.0;
  for (float f : v) sum_sq += double(f) * f;
  const float norm = (float)std::sqrt(sum_sq);
  const float inv_norm = norm > 0.0f ? 1.0f / norm : 0.0f;

  Slot self;
  auto existing = slot_of_.find(id);
  if (existing != slot_of_.end()) {
    self = existing->second;
    for (size_t i = 0; i < dimension_; ++i) {
      vectors_[(size_t)self * dimension_ + i] = v[i] * inv_norm;
    }
    for (auto& layer : links_[self]) layer.clear();
  } else {
    self = (Slot)ids_.size();
    ids_.push_back(id);
    for (size_t i = 0; i < dimension_; ++i) {
      vectors_.push_back(v[i] * inv_norm);
    }
    links_.emplace_back();
    slot_of_[id] = self;
  }
  if ((int)links_[self].size() < target_layer + 1) links_[self].resize(target_layer + 1);

  if (ids_.size() == 1) {
    entry_ = self;
    max_layer_ = target_layer;
    return Ok();
  }

  const float* q = vec(self);
  float curr_dist = distance(q, entry_);
  Slot curr_ep = greedy_descend(q, entry_, curr_dist, max_layer_, target_layer);

  std::vector<Cand> found;
  for (int l = std::min(target_layer, max_layer_); l >= 0; --l) {
    search_layer(q, curr_ep, curr_dist, ef_construction_, l, found);

    std::vector<Cand> others;
    others.reserve(found.size());
    for (const auto& f : found) if (f.second != self) others.push_back(f);
    for (Slot other : select_neighbours(others, M_)) {
      links_[self][l].push_back(other);
      if ((int)links_[other].size() <= l) links_[other].resize(l + 1);
      auto& list = links_[other][l];
      list.push_back(self);

      const size_t cap = (l == 0) ? M_ * 2 : M_;
      if (list.size() > cap) {
        std::vector<Cand> ranked;
        ranked.reserve(list.size());
        for (Slot n2 : list) ranked.push_back({distance(vec(other), n2), n2});
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        list = select_neighbours(ranked, cap);
      }
    }
    if (!found.empty()) { curr_dist = found.front().first; curr_ep = found.front().second; }
  }

  if (target_layer > max_layer_) {
    max_layer_ = target_layer;
    entry_ = self;
  }

  return Ok();
}

std::vector<std::pair<ObjectID, float>> HNSWVectorIndex::search_knn(const std::vector<float>& query, size_t k, size_t ef_search) const {
  std::vector<std::pair<ObjectID, float>> results;
  if (ids_.empty() || query.size() != dimension_) return results;

  double sum_sq = 0.0;
  for (float f : query) sum_sq += double(f) * f;
  const float norm = (float)std::sqrt(sum_sq);
  const float inv_norm = norm > 0.0f ? 1.0f / norm : 0.0f;
  std::vector<float> q_norm(dimension_);
  for (size_t i = 0; i < dimension_; ++i) q_norm[i] = query[i] * inv_norm;

  float curr_dist = distance(q_norm.data(), entry_);
  const Slot ep = greedy_descend(q_norm.data(), entry_, curr_dist, max_layer_, 0);

  const size_t ef = ef_search ? std::max(ef_search, k) : std::max(k * 2, (size_t)16);

  std::vector<Cand> found;
  search_layer(q_norm.data(), ep, curr_dist, ef, 0, found);

  for (const auto& [d, s] : found) {
    if (results.size() >= k) break;
    results.push_back({ids_[s], d});
  }
  return results;
}

}  // namespace aios::memory
