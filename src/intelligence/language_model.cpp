// The machine's own language model. See the header for what this is and, more
// importantly, what it is not: a small transformer trained here, on a corpus
// this machine writes about itself, that maps a request onto one of the console
// commands it was trained on. No BLAS, no autograd, no external runtime, no
// host-only header — the same source links into the bare-metal image.
#include "aios/intelligence/language_model.hpp"
#include "aios/intelligence/world_model.hpp"
#include "aios/compute/compute_graph.hpp"
#include "aios/compute/scheduler.hpp"
#include "aios/memory/memory_fabric.hpp"

#include <cmath>
#include <cstring>
#include <algorithm>

namespace aios::intelligence {
namespace {

// ---- the substrate's store, if it registered one ---------------------------
LmStoreRead g_read = nullptr;
LmStoreWrite g_write = nullptr;

// ---- little-endian primitives, so a blob written on one host reads on another
void put_u32(std::string& out, uint32_t v) {
  char b[4] = {(char)(v & 0xFF), (char)((v >> 8) & 0xFF), (char)((v >> 16) & 0xFF), (char)((v >> 24) & 0xFF)};
  out.append(b, 4);
}
void put_u64(std::string& out, uint64_t v) { put_u32(out, (uint32_t)(v & 0xFFFFFFFFu)); put_u32(out, (uint32_t)(v >> 32)); }
void put_f32(std::string& out, float f) { uint32_t u; std::memcpy(&u, &f, 4); put_u32(out, u); }
bool get_u32(const std::string& b, size_t& o, uint32_t& v) {
  if (o + 4 > b.size()) return false;
  v = (uint8_t)b[o] | ((uint32_t)(uint8_t)b[o + 1] << 8) | ((uint32_t)(uint8_t)b[o + 2] << 16) | ((uint32_t)(uint8_t)b[o + 3] << 24);
  o += 4; return true;
}
bool get_u64(const std::string& b, size_t& o, uint64_t& v) {
  uint32_t lo, hi;
  if (!get_u32(b, o, lo) || !get_u32(b, o, hi)) return false;
  v = (uint64_t)lo | ((uint64_t)hi << 32); return true;
}
bool get_f32(const std::string& b, size_t& o, float& f) {
  uint32_t u; if (!get_u32(b, o, u)) return false; std::memcpy(&f, &u, 4); return true;
}
void put_str(std::string& out, const std::string& s) { put_u32(out, (uint32_t)s.size()); out += s; }
bool get_str(const std::string& b, size_t& o, std::string& s) {
  uint32_t n; if (!get_u32(b, o, n)) return false;
  if (o + n > b.size()) return false;
  s.assign(b, o, n); o += n; return true;
}

// A deterministic PRNG. The seed is in the header's signature because a model
// that cannot be re-derived from a seed cannot be checked by anyone.
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  uint32_t next() {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return (uint32_t)(s >> 32);
  }
  float uniform() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }
  // Box-Muller, so the initialisation is actually normal and not a triangle.
  float normal() {
    float u1 = uniform(), u2 = uniform();
    if (u1 < 1e-7f) u1 = 1e-7f;
    return std::sqrt(-2.0f * std::log(u1)) * std::cos(6.2831853f * u2);
  }
};

}  // namespace

void set_lm_store(LmStoreRead r, LmStoreWrite w) { g_read = r; g_write = w; }
bool lm_store_is_live() { return g_read != nullptr && g_write != nullptr; }

// ---------------------------------------------------------------------------
// Byte-level BPE
// ---------------------------------------------------------------------------
// Every byte is a token, so nothing can be out of vocabulary. Merges are learned
// by counting adjacent pairs across the corpus and joining the most frequent,
// repeatedly, which is the whole of BPE.
void ByteBpeTokenizer::train(const std::vector<std::string>& corpus, size_t target_vocab) {
  merges_.clear();
  vocab_.clear();
  vocab_.reserve(target_vocab ? target_vocab : 256);
  for (int b = 0; b < 256; ++b) vocab_.push_back(std::string(1, (char)b));
  if (target_vocab <= 256) return;

  // The corpus as sequences of token ids, refined in place as merges are added.
  std::vector<std::vector<int>> seqs;
  seqs.reserve(corpus.size());
  for (const auto& line : corpus) {
    std::vector<int> s;
    s.reserve(line.size());
    for (unsigned char c : line) s.push_back((int)c);
    if (s.size() >= 2) seqs.push_back(std::move(s));
  }
  if (seqs.empty()) return;

  while (vocab_.size() < target_vocab) {
    // Count every adjacent pair. A flat vector keyed by (a,b) would be
    // vocab^2 and mostly empty, so this walks and accumulates.
    struct Count { int a, b; uint32_t n; };
    std::vector<Count> counts;
    counts.reserve(4096);
    for (const auto& s : seqs) {
      for (size_t i = 0; i + 1 < s.size(); ++i) {
        const int a = s[i], b = s[i + 1];
        bool found = false;
        for (auto& c : counts) {
          if (c.a == a && c.b == b) { ++c.n; found = true; break; }
        }
        if (!found) counts.push_back({a, b, 1});
      }
    }
    if (counts.empty()) break;
    size_t best = 0;
    for (size_t i = 1; i < counts.size(); ++i) if (counts[i].n > counts[best].n) best = i;
    if (counts[best].n < 2) break;   // nothing repeats: there is nothing left to learn

    const int a = counts[best].a, b = counts[best].b;
    const int id = (int)vocab_.size();
    vocab_.push_back(vocab_[(size_t)a] + vocab_[(size_t)b]);
    merges_.push_back({a, b, id});

    for (auto& s : seqs) {
      std::vector<int> out;
      out.reserve(s.size());
      for (size_t i = 0; i < s.size();) {
        if (i + 1 < s.size() && s[i] == a && s[i + 1] == b) { out.push_back(id); i += 2; }
        else { out.push_back(s[i]); ++i; }
      }
      s.swap(out);
    }
  }
}

int ByteBpeTokenizer::rank_of(int a, int b) const {
  for (size_t i = 0; i < merges_.size(); ++i) if (merges_[i].a == a && merges_[i].b == b) return (int)i;
  return -1;
}

std::vector<int> ByteBpeTokenizer::encode(const std::string& text) const {
  std::vector<int> ids;
  ids.reserve(text.size());
  for (unsigned char c : text) ids.push_back((int)c);
  if (merges_.empty()) return ids;
  // Apply the EARLIEST-learned applicable merge each pass, which is what makes
  // encoding agree with training rather than merging greedily left to right.
  for (;;) {
    int best_rank = -1;
    size_t best_at = 0;
    for (size_t i = 0; i + 1 < ids.size(); ++i) {
      const int r = rank_of(ids[i], ids[i + 1]);
      if (r < 0) continue;
      if (best_rank < 0 || r < best_rank) { best_rank = r; best_at = i; }
    }
    if (best_rank < 0) break;
    ids[best_at] = merges_[(size_t)best_rank].id;
    ids.erase(ids.begin() + (long)best_at + 1);
  }
  return ids;
}

std::string ByteBpeTokenizer::decode(const std::vector<int>& ids) const {
  std::string out;
  for (int id : ids) {
    if (id >= 0 && (size_t)id < vocab_.size()) out += vocab_[(size_t)id];
  }
  return out;
}

void ByteBpeTokenizer::serialize(std::string& out) const {
  put_u32(out, 0x42504531u);              // "BPE1"
  put_u32(out, (uint32_t)vocab_.size());
  for (const auto& v : vocab_) put_str(out, v);
  put_u32(out, (uint32_t)merges_.size());
  for (const auto& m : merges_) { put_u32(out, (uint32_t)m.a); put_u32(out, (uint32_t)m.b); put_u32(out, (uint32_t)m.id); }
}

bool ByteBpeTokenizer::deserialize(const std::string& blob, size_t& off) {
  uint32_t magic = 0, n = 0;
  if (!get_u32(blob, off, magic) || magic != 0x42504531u) return false;
  if (!get_u32(blob, off, n)) return false;
  vocab_.clear();
  vocab_.reserve(n);
  for (uint32_t i = 0; i < n; ++i) { std::string s; if (!get_str(blob, off, s)) return false; vocab_.push_back(std::move(s)); }
  if (!get_u32(blob, off, n)) return false;
  merges_.clear();
  merges_.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t a, b, id;
    if (!get_u32(blob, off, a) || !get_u32(blob, off, b) || !get_u32(blob, off, id)) return false;
    merges_.push_back({(int)a, (int)b, (int)id});
  }
  return true;
}


// ---------------------------------------------------------------------------
// A pre-LN causal transformer decoder, forward and backward, written out.
// ---------------------------------------------------------------------------
// Layout convention used everywhere below: a weight W maps D_in -> D_out and is
// stored row-major as W[i * D_out + j], so y[j] = sum_i x[i] * W[i*D_out + j].
// Every loop in the backward pass is the derivative of exactly one loop in the
// forward pass, in the same order, so the two can be read against each other.
namespace {

constexpr float kEps = 1e-5f;

inline float gelu(float x) {
  // The tanh approximation, so the derivative below is a closed form.
  const float c = 0.7978845608f;  // sqrt(2/pi)
  const float inner = c * (x + 0.044715f * x * x * x);
  return 0.5f * x * (1.0f + std::tanh(inner));
}
inline float dgelu(float x) {
  const float c = 0.7978845608f;
  const float x3 = x * x * x;
  const float inner = c * (x + 0.044715f * x3);
  const float t = std::tanh(inner);
  const float dinner = c * (1.0f + 3.0f * 0.044715f * x * x);
  return 0.5f * (1.0f + t) + 0.5f * x * (1.0f - t * t) * dinner;
}

// y = g * (x - mu) / sqrt(var + eps) + b, and what the backward pass needs.
struct NormStat { float mu, inv; };

}  // namespace

// Everything one forward pass leaves behind for the backward pass. Sized for
// one sequence; a batch runs these one sequence at a time and accumulates.
struct TinyTransformerLM::Cache {
  int T = 0;
  std::vector<int> ids;
  std::vector<float> x0;                       // embeddings, T*D
  struct LayerCache {
    std::vector<float> pre;                    // x entering the layer, T*D
    std::vector<float> h1;                     // LN1 out, T*D
    std::vector<NormStat> n1;                  // T
    std::vector<float> q, k, v;                // T*D each
    std::vector<float> att;                    // H*T*T, post-softmax
    std::vector<float> o;                      // attention out (concat heads), T*D
    std::vector<float> mid;                    // x after the attention residual, T*D
    std::vector<float> h2;                     // LN2 out, T*D
    std::vector<NormStat> n2;                  // T
    std::vector<float> ff_pre;                 // pre-activation, T*F
    std::vector<float> ff;                     // post-GELU, T*F
  };
  std::vector<LayerCache> L;
  std::vector<float> xf;                       // final LN out, T*D
  std::vector<NormStat> nf;                    // T
  std::vector<float> probs;                    // T*V softmax of logits
  float loss = 0.0f;
};

void TinyTransformerLM::init(int vocab, const TinyLmConfig& cfg, uint32_t seed) {
  cfg_ = cfg;
  vocab_ = vocab;
  steps_ = 0;
  last_loss_ = 0.0f;
  const int D = cfg_.d_model, F = cfg_.d_ff;
  Rng rng(seed);
  // Small normal init scaled by fan-in; the residual stream stays sane at this
  // depth without any of the fancier schemes.
  auto fill = [&](std::vector<float>& w, size_t n, float sd) {
    w.assign(n, 0.0f);
    for (size_t i = 0; i < n; ++i) w[i] = rng.normal() * sd;
  };
  fill(tok_emb_, (size_t)vocab_ * D, 0.02f);
  fill(pos_emb_, (size_t)cfg_.block * D, 0.02f);
  gf_.assign(D, 1.0f); bf_.assign(D, 0.0f);
  fill(head_, (size_t)D * vocab_, 1.0f / std::sqrt((float)D));
  head_b_.assign(vocab_, 0.0f);
  layers_.assign((size_t)cfg_.n_layer, Layer{});
  for (auto& L : layers_) {
    L.g1.assign(D, 1.0f); L.b1.assign(D, 0.0f);
    L.g2.assign(D, 1.0f); L.b2.assign(D, 0.0f);
    const float sd = 1.0f / std::sqrt((float)D);
    fill(L.Wq, (size_t)D * D, sd); fill(L.Wk, (size_t)D * D, sd);
    fill(L.Wv, (size_t)D * D, sd); fill(L.Wo, (size_t)D * D, sd);
    L.bq.assign(D, 0.0f); L.bk.assign(D, 0.0f); L.bv.assign(D, 0.0f); L.bo.assign(D, 0.0f);
    fill(L.W1, (size_t)D * F, sd);
    L.b1f.assign(F, 0.0f);
    fill(L.W2, (size_t)F * D, 1.0f / std::sqrt((float)F));
    L.b2f.assign(D, 0.0f);
  }
  const size_t n = 0;
  (void)n;
  size_t total = 0;
  for (size_t sz : param_sizes()) total += sz;
  m_.assign(total, 0.0f);
  v_.assign(total, 0.0f);
}

std::vector<float*> TinyTransformerLM::params() {
  std::vector<float*> p{tok_emb_.data(), pos_emb_.data(), gf_.data(), bf_.data(), head_.data(), head_b_.data()};
  for (auto& L : layers_) {
    p.insert(p.end(), {L.g1.data(), L.b1.data(), L.g2.data(), L.b2.data(),
                       L.Wq.data(), L.Wk.data(), L.Wv.data(), L.Wo.data(),
                       L.bq.data(), L.bk.data(), L.bv.data(), L.bo.data(),
                       L.W1.data(), L.b1f.data(), L.W2.data(), L.b2f.data()});
  }
  return p;
}

std::vector<const float*> TinyTransformerLM::params() const {
  std::vector<const float*> p{tok_emb_.data(), pos_emb_.data(), gf_.data(), bf_.data(), head_.data(), head_b_.data()};
  for (const auto& L : layers_) {
    p.insert(p.end(), {L.g1.data(), L.b1.data(), L.g2.data(), L.b2.data(),
                       L.Wq.data(), L.Wk.data(), L.Wv.data(), L.Wo.data(),
                       L.bq.data(), L.bk.data(), L.bv.data(), L.bo.data(),
                       L.W1.data(), L.b1f.data(), L.W2.data(), L.b2f.data()});
  }
  return p;
}

std::vector<size_t> TinyTransformerLM::param_sizes() const {
  const size_t D = (size_t)cfg_.d_model, F = (size_t)cfg_.d_ff, V = (size_t)vocab_;
  std::vector<size_t> s{V * D, (size_t)cfg_.block * D, D, D, D * V, V};
  for (int i = 0; i < cfg_.n_layer; ++i) {
    const size_t l[] = {D, D, D, D, D * D, D * D, D * D, D * D, D, D, D, D, D * F, F, F * D, D};
    s.insert(s.end(), l, l + 16);
  }
  return s;
}

size_t TinyTransformerLM::parameter_count() const {
  size_t n = 0;
  for (size_t s : param_sizes()) n += s;
  return n;
}

void TinyTransformerLM::forward(const std::vector<int>& ids, Cache& c) const {
  const int D = cfg_.d_model, H = cfg_.n_head, dh = D / H, F = cfg_.d_ff, V = vocab_;
  const int T = (int)std::min((size_t)cfg_.block, ids.size());
  c.T = T;
  c.ids.assign(ids.begin(), ids.begin() + T);
  c.L.assign((size_t)cfg_.n_layer, Cache::LayerCache{});

  c.x0.assign((size_t)T * D, 0.0f);
  for (int t = 0; t < T; ++t) {
    const int id = std::clamp(c.ids[(size_t)t], 0, V - 1);
    for (int j = 0; j < D; ++j) {
      c.x0[(size_t)t * D + j] = tok_emb_[(size_t)id * D + j] + pos_emb_[(size_t)t * D + j];
    }
  }

  std::vector<float> x = c.x0;
  for (int li = 0; li < cfg_.n_layer; ++li) {
    const Layer& L = layers_[(size_t)li];
    Cache::LayerCache& lc = c.L[(size_t)li];
    lc.pre = x;

    // ---- LN1
    lc.h1.assign((size_t)T * D, 0.0f);
    lc.n1.assign((size_t)T, {0.0f, 0.0f});
    for (int t = 0; t < T; ++t) {
      float mu = 0.0f;
      for (int j = 0; j < D; ++j) mu += x[(size_t)t * D + j];
      mu /= (float)D;
      float var = 0.0f;
      for (int j = 0; j < D; ++j) { const float d = x[(size_t)t * D + j] - mu; var += d * d; }
      var /= (float)D;
      const float inv = 1.0f / std::sqrt(var + kEps);
      lc.n1[(size_t)t] = {mu, inv};
      for (int j = 0; j < D; ++j) {
        lc.h1[(size_t)t * D + j] = L.g1[(size_t)j] * (x[(size_t)t * D + j] - mu) * inv + L.b1[(size_t)j];
      }
    }

    // ---- q, k, v
    lc.q.assign((size_t)T * D, 0.0f); lc.k = lc.q; lc.v = lc.q;
    for (int t = 0; t < T; ++t) {
      for (int j = 0; j < D; ++j) {
        float aq = L.bq[(size_t)j], ak = L.bk[(size_t)j], av = L.bv[(size_t)j];
        for (int i = 0; i < D; ++i) {
          const float h = lc.h1[(size_t)t * D + i];
          aq += h * L.Wq[(size_t)i * D + j];
          ak += h * L.Wk[(size_t)i * D + j];
          av += h * L.Wv[(size_t)i * D + j];
        }
        lc.q[(size_t)t * D + j] = aq;
        lc.k[(size_t)t * D + j] = ak;
        lc.v[(size_t)t * D + j] = av;
      }
    }

    // ---- causal attention, per head
    lc.att.assign((size_t)H * T * T, 0.0f);
    lc.o.assign((size_t)T * D, 0.0f);
    const float scale = 1.0f / std::sqrt((float)dh);
    for (int h = 0; h < H; ++h) {
      for (int t = 0; t < T; ++t) {
        float best = -1e30f;
        for (int s2 = 0; s2 <= t; ++s2) {
          float dot = 0.0f;
          for (int j = 0; j < dh; ++j) {
            dot += lc.q[(size_t)t * D + h * dh + j] * lc.k[(size_t)s2 * D + h * dh + j];
          }
          const float sc = dot * scale;
          lc.att[((size_t)h * T + t) * T + s2] = sc;
          if (sc > best) best = sc;
        }
        float sum = 0.0f;
        for (int s2 = 0; s2 <= t; ++s2) {
          const float e = std::exp(lc.att[((size_t)h * T + t) * T + s2] - best);
          lc.att[((size_t)h * T + t) * T + s2] = e;
          sum += e;
        }
        const float inv = sum > 0.0f ? 1.0f / sum : 0.0f;
        for (int s2 = 0; s2 <= t; ++s2) lc.att[((size_t)h * T + t) * T + s2] *= inv;
        for (int j = 0; j < dh; ++j) {
          float acc = 0.0f;
          for (int s2 = 0; s2 <= t; ++s2) {
            acc += lc.att[((size_t)h * T + t) * T + s2] * lc.v[(size_t)s2 * D + h * dh + j];
          }
          lc.o[(size_t)t * D + h * dh + j] = acc;
        }
      }
    }

    // ---- output projection + residual
    lc.mid.assign((size_t)T * D, 0.0f);
    for (int t = 0; t < T; ++t) {
      for (int j = 0; j < D; ++j) {
        float acc = L.bo[(size_t)j];
        for (int i = 0; i < D; ++i) acc += lc.o[(size_t)t * D + i] * L.Wo[(size_t)i * D + j];
        lc.mid[(size_t)t * D + j] = x[(size_t)t * D + j] + acc;
      }
    }

    // ---- LN2
    lc.h2.assign((size_t)T * D, 0.0f);
    lc.n2.assign((size_t)T, {0.0f, 0.0f});
    for (int t = 0; t < T; ++t) {
      float mu = 0.0f;
      for (int j = 0; j < D; ++j) mu += lc.mid[(size_t)t * D + j];
      mu /= (float)D;
      float var = 0.0f;
      for (int j = 0; j < D; ++j) { const float d = lc.mid[(size_t)t * D + j] - mu; var += d * d; }
      var /= (float)D;
      const float inv = 1.0f / std::sqrt(var + kEps);
      lc.n2[(size_t)t] = {mu, inv};
      for (int j = 0; j < D; ++j) {
        lc.h2[(size_t)t * D + j] = L.g2[(size_t)j] * (lc.mid[(size_t)t * D + j] - mu) * inv + L.b2[(size_t)j];
      }
    }

    // ---- feed-forward + residual
    lc.ff_pre.assign((size_t)T * F, 0.0f);
    lc.ff.assign((size_t)T * F, 0.0f);
    x.assign((size_t)T * D, 0.0f);
    for (int t = 0; t < T; ++t) {
      for (int f = 0; f < F; ++f) {
        float acc = L.b1f[(size_t)f];
        for (int i = 0; i < D; ++i) acc += lc.h2[(size_t)t * D + i] * L.W1[(size_t)i * F + f];
        lc.ff_pre[(size_t)t * F + f] = acc;
        lc.ff[(size_t)t * F + f] = gelu(acc);
      }
      for (int j = 0; j < D; ++j) {
        float acc = L.b2f[(size_t)j];
        for (int f = 0; f < F; ++f) acc += lc.ff[(size_t)t * F + f] * L.W2[(size_t)f * D + j];
        x[(size_t)t * D + j] = lc.mid[(size_t)t * D + j] + acc;
      }
    }
  }

  // ---- final LN, logits, softmax, loss over next-token targets
  c.xf.assign((size_t)T * D, 0.0f);
  c.nf.assign((size_t)T, {0.0f, 0.0f});
  for (int t = 0; t < T; ++t) {
    float mu = 0.0f;
    for (int j = 0; j < D; ++j) mu += x[(size_t)t * D + j];
    mu /= (float)D;
    float var = 0.0f;
    for (int j = 0; j < D; ++j) { const float d = x[(size_t)t * D + j] - mu; var += d * d; }
    var /= (float)D;
    const float inv = 1.0f / std::sqrt(var + kEps);
    c.nf[(size_t)t] = {mu, inv};
    for (int j = 0; j < D; ++j) {
      c.xf[(size_t)t * D + j] = gf_[(size_t)j] * (x[(size_t)t * D + j] - mu) * inv + bf_[(size_t)j];
    }
  }

  c.probs.assign((size_t)T * V, 0.0f);
  c.loss = 0.0f;
  int counted = 0;
  for (int t = 0; t < T; ++t) {
    float best = -1e30f;
    for (int w = 0; w < V; ++w) {
      float acc = head_b_[(size_t)w];
      for (int i = 0; i < D; ++i) acc += c.xf[(size_t)t * D + i] * head_[(size_t)i * V + w];
      c.probs[(size_t)t * V + w] = acc;
      if (acc > best) best = acc;
    }
    float sum = 0.0f;
    for (int w = 0; w < V; ++w) {
      const float e = std::exp(c.probs[(size_t)t * V + w] - best);
      c.probs[(size_t)t * V + w] = e;
      sum += e;
    }
    const float inv = sum > 0.0f ? 1.0f / sum : 0.0f;
    for (int w = 0; w < V; ++w) c.probs[(size_t)t * V + w] *= inv;
    if (t + 1 < T) {
      const int tgt = std::clamp(c.ids[(size_t)t + 1], 0, V - 1);
      const float p = c.probs[(size_t)t * V + tgt];
      c.loss += -std::log(p > 1e-12f ? p : 1e-12f);
      ++counted;
    }
  }
  if (counted) c.loss /= (float)counted;
}

float TinyTransformerLM::sequence_loss(const std::vector<int>& ids) const {
  if (!ready() || ids.size() < 2) return 0.0f;
  Cache c;
  forward(ids, c);
  return c.loss;
}


// ---------------------------------------------------------------------------
// One AdamW step over a batch. The backward pass below is the derivative of
// each loop in forward(), in the same order, reading bottom to top.
// ---------------------------------------------------------------------------
std::vector<std::vector<float>> TinyTransformerLM::zero_gradients() const {
  const std::vector<size_t> sizes = param_sizes();
  std::vector<std::vector<float>> g(sizes.size());
  for (size_t i = 0; i < sizes.size(); ++i) g[i].assign(sizes[i], 0.0f);
  return g;
}

float TinyTransformerLM::accumulate_gradients(const std::vector<int>& ids,
                                              std::vector<std::vector<float>>& g) const {
  const int D = cfg_.d_model, H = cfg_.n_head, dh = D / H, F = cfg_.d_ff, V = vocab_;
  // Names for the slots, so the code below reads as maths and not as indices.
  enum { G_TOK = 0, G_POS, G_GF, G_BF, G_HEAD, G_HEADB, G_LAYER0 };
  auto lslot = [&](int li, int which) -> std::vector<float>& {
    return g[(size_t)G_LAYER0 + (size_t)li * 16 + (size_t)which];
  };
  enum { L_G1 = 0, L_B1, L_G2, L_B2, L_WQ, L_WK, L_WV, L_WO, L_BQ, L_BK, L_BV, L_BO, L_W1, L_B1F, L_W2, L_B2F };
  if (!ready() || ids.size() < 2) return 0.0f;
  {
    Cache c;
    forward(ids, c);
    const int T = c.T;
    if (T < 2) return 0.0f;
    const float norm = 1.0f / (float)(T - 1);

    // ---- logits: dL/dlogit = (p - onehot) / (T-1)
    std::vector<float> dxf((size_t)T * D, 0.0f);
    for (int t = 0; t + 1 < T; ++t) {
      const int tgt = std::clamp(c.ids[(size_t)t + 1], 0, V - 1);
      for (int w = 0; w < V; ++w) {
        float d = c.probs[(size_t)t * V + w];
        if (w == tgt) d -= 1.0f;
        d *= norm;
        if (d == 0.0f) continue;
        g[G_HEADB][(size_t)w] += d;
        for (int i = 0; i < D; ++i) {
          g[G_HEAD][(size_t)i * V + w] += c.xf[(size_t)t * D + i] * d;
          dxf[(size_t)t * D + i] += head_[(size_t)i * V + w] * d;
        }
      }
    }

    // ---- final layernorm
    // Reconstructing the normalised value from the cache costs one divide and
    // saves keeping another T*D array per layer per sequence.
    std::vector<float> dx((size_t)T * D, 0.0f);
    for (int t = 0; t < T; ++t) {
      const NormStat n = c.nf[(size_t)t];
      float sum_d = 0.0f, sum_dn = 0.0f;
      for (int j = 0; j < D; ++j) {
        const float xh = (c.xf[(size_t)t * D + j] - bf_[(size_t)j]) / (gf_[(size_t)j] != 0.0f ? gf_[(size_t)j] : 1.0f);
        const float d = dxf[(size_t)t * D + j];
        g[G_GF][(size_t)j] += d * xh;
        g[G_BF][(size_t)j] += d;
        const float dn = d * gf_[(size_t)j];
        sum_d += dn;
        sum_dn += dn * xh;
      }
      for (int j = 0; j < D; ++j) {
        const float xh = (c.xf[(size_t)t * D + j] - bf_[(size_t)j]) / (gf_[(size_t)j] != 0.0f ? gf_[(size_t)j] : 1.0f);
        const float dn = dxf[(size_t)t * D + j] * gf_[(size_t)j];
        dx[(size_t)t * D + j] = n.inv * (dn - (sum_d + xh * sum_dn) / (float)D);
      }
    }

    // ---- the layers, last to first
    for (int li = cfg_.n_layer - 1; li >= 0; --li) {
      const Layer& L = layers_[(size_t)li];
      const Cache::LayerCache& lc = c.L[(size_t)li];

      // feed-forward residual: x = mid + ffn(h2)
      std::vector<float> dmid = dx;              // the residual path
      std::vector<float> dh2((size_t)T * D, 0.0f);
      for (int t = 0; t < T; ++t) {
        std::vector<float> dff((size_t)F, 0.0f);
        for (int j = 0; j < D; ++j) {
          const float d = dx[(size_t)t * D + j];
          if (d == 0.0f) continue;
          lslot(li, L_B2F)[(size_t)j] += d;
          for (int f = 0; f < F; ++f) {
            lslot(li, L_W2)[(size_t)f * D + j] += lc.ff[(size_t)t * F + f] * d;
            dff[(size_t)f] += L.W2[(size_t)f * D + j] * d;
          }
        }
        for (int f = 0; f < F; ++f) {
          const float d = dff[(size_t)f] * dgelu(lc.ff_pre[(size_t)t * F + f]);
          if (d == 0.0f) continue;
          lslot(li, L_B1F)[(size_t)f] += d;
          for (int i = 0; i < D; ++i) {
            lslot(li, L_W1)[(size_t)i * F + f] += lc.h2[(size_t)t * D + i] * d;
            dh2[(size_t)t * D + i] += L.W1[(size_t)i * F + f] * d;
          }
        }
      }

      // LN2
      for (int t = 0; t < T; ++t) {
        const NormStat n = lc.n2[(size_t)t];
        float sum_d = 0.0f, sum_dn = 0.0f;
        for (int j = 0; j < D; ++j) {
          const float xh = (lc.mid[(size_t)t * D + j] - n.mu) * n.inv;
          const float d = dh2[(size_t)t * D + j];
          lslot(li, L_G2)[(size_t)j] += d * xh;
          lslot(li, L_B2)[(size_t)j] += d;
          const float dn = d * L.g2[(size_t)j];
          sum_d += dn;
          sum_dn += dn * xh;
        }
        for (int j = 0; j < D; ++j) {
          const float xh = (lc.mid[(size_t)t * D + j] - n.mu) * n.inv;
          const float dn = dh2[(size_t)t * D + j] * L.g2[(size_t)j];
          dmid[(size_t)t * D + j] += n.inv * (dn - (sum_d + xh * sum_dn) / (float)D);
        }
      }

      // attention residual: mid = pre + o·Wo
      std::vector<float> dpre = dmid;
      std::vector<float> dO((size_t)T * D, 0.0f);
      for (int t = 0; t < T; ++t) {
        for (int j = 0; j < D; ++j) {
          const float d = dmid[(size_t)t * D + j];
          if (d == 0.0f) continue;
          lslot(li, L_BO)[(size_t)j] += d;
          for (int i = 0; i < D; ++i) {
            lslot(li, L_WO)[(size_t)i * D + j] += lc.o[(size_t)t * D + i] * d;
            dO[(size_t)t * D + i] += L.Wo[(size_t)i * D + j] * d;
          }
        }
      }

      // attention itself
      std::vector<float> dq((size_t)T * D, 0.0f), dk((size_t)T * D, 0.0f), dv((size_t)T * D, 0.0f);
      const float scale = 1.0f / std::sqrt((float)dh);
      for (int h = 0; h < H; ++h) {
        for (int t = 0; t < T; ++t) {
          // dA[s] from dO, and dv from the same product
          std::vector<float> dA((size_t)t + 1, 0.0f);
          for (int s2 = 0; s2 <= t; ++s2) {
            const float a = lc.att[((size_t)h * T + t) * T + s2];
            float acc = 0.0f;
            for (int j = 0; j < dh; ++j) {
              const float d = dO[(size_t)t * D + h * dh + j];
              acc += d * lc.v[(size_t)s2 * D + h * dh + j];
              dv[(size_t)s2 * D + h * dh + j] += a * d;
            }
            dA[(size_t)s2] = acc;
          }
          // softmax jacobian: ds = a * (dA - sum_j a_j dA_j)
          float dot = 0.0f;
          for (int s2 = 0; s2 <= t; ++s2) dot += lc.att[((size_t)h * T + t) * T + s2] * dA[(size_t)s2];
          for (int s2 = 0; s2 <= t; ++s2) {
            const float a = lc.att[((size_t)h * T + t) * T + s2];
            const float ds = a * (dA[(size_t)s2] - dot) * scale;
            if (ds == 0.0f) continue;
            for (int j = 0; j < dh; ++j) {
              dq[(size_t)t * D + h * dh + j] += ds * lc.k[(size_t)s2 * D + h * dh + j];
              dk[(size_t)s2 * D + h * dh + j] += ds * lc.q[(size_t)t * D + h * dh + j];
            }
          }
        }
      }

      // q, k, v projections
      std::vector<float> dh1((size_t)T * D, 0.0f);
      for (int t = 0; t < T; ++t) {
        for (int j = 0; j < D; ++j) {
          const float gq = dq[(size_t)t * D + j], gk = dk[(size_t)t * D + j], gv = dv[(size_t)t * D + j];
          if (gq != 0.0f) lslot(li, L_BQ)[(size_t)j] += gq;
          if (gk != 0.0f) lslot(li, L_BK)[(size_t)j] += gk;
          if (gv != 0.0f) lslot(li, L_BV)[(size_t)j] += gv;
          for (int i = 0; i < D; ++i) {
            const float h1 = lc.h1[(size_t)t * D + i];
            lslot(li, L_WQ)[(size_t)i * D + j] += h1 * gq;
            lslot(li, L_WK)[(size_t)i * D + j] += h1 * gk;
            lslot(li, L_WV)[(size_t)i * D + j] += h1 * gv;
            dh1[(size_t)t * D + i] += L.Wq[(size_t)i * D + j] * gq + L.Wk[(size_t)i * D + j] * gk +
                                      L.Wv[(size_t)i * D + j] * gv;
          }
        }
      }

      // LN1
      for (int t = 0; t < T; ++t) {
        const NormStat n = lc.n1[(size_t)t];
        float sum_d = 0.0f, sum_dn = 0.0f;
        for (int j = 0; j < D; ++j) {
          const float xh = (lc.pre[(size_t)t * D + j] - n.mu) * n.inv;
          const float d = dh1[(size_t)t * D + j];
          lslot(li, L_G1)[(size_t)j] += d * xh;
          lslot(li, L_B1)[(size_t)j] += d;
          const float dn = d * L.g1[(size_t)j];
          sum_d += dn;
          sum_dn += dn * xh;
        }
        for (int j = 0; j < D; ++j) {
          const float xh = (lc.pre[(size_t)t * D + j] - n.mu) * n.inv;
          const float dn = dh1[(size_t)t * D + j] * L.g1[(size_t)j];
          dpre[(size_t)t * D + j] += n.inv * (dn - (sum_d + xh * sum_dn) / (float)D);
        }
      }
      dx.swap(dpre);
    }

    // ---- embeddings
    for (int t = 0; t < T; ++t) {
      const int id = std::clamp(c.ids[(size_t)t], 0, V - 1);
      for (int j = 0; j < D; ++j) {
        const float d = dx[(size_t)t * D + j];
        g[G_TOK][(size_t)id * D + j] += d;
        g[G_POS][(size_t)t * D + j] += d;
      }
    }
    return c.loss;
  }
}

float TinyTransformerLM::train_step(const std::vector<std::vector<int>>& batch, float lr) {
  if (!ready() || batch.empty()) return last_loss_;
  std::vector<std::vector<float>> g = zero_gradients();
  float total = 0.0f;
  int sequences = 0;
  for (const auto& ids : batch) {
    if (ids.size() < 2) continue;
    total += accumulate_gradients(ids, g);
    ++sequences;
  }
  if (!sequences) return last_loss_;
  const float inv_batch = 1.0f / (float)sequences;
  last_loss_ = total * inv_batch;

  // ---- AdamW, decoupled decay, bias-corrected
  ++steps_;
  const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f, decay = 0.01f;
  const float c1 = 1.0f - std::pow(b1, (float)steps_);
  const float c2 = 1.0f - std::pow(b2, (float)steps_);
  std::vector<float*> ps = params();
  size_t at = 0;
  for (size_t pi = 0; pi < ps.size(); ++pi) {
    float* w = ps[pi];
    const std::vector<float>& gr = g[pi];
    for (size_t i = 0; i < gr.size(); ++i, ++at) {
      const float grad = gr[i] * inv_batch;
      m_[at] = b1 * m_[at] + (1.0f - b1) * grad;
      v_[at] = b2 * v_[at] + (1.0f - b2) * grad * grad;
      const float mh = m_[at] / c1;
      const float vh = v_[at] / c2;
      w[i] -= lr * (mh / (std::sqrt(vh) + eps) + decay * w[i]);
    }
  }
  return last_loss_;
}

// ---------------------------------------------------------------------------
// The same training step, as a ComputeGraph the kernel's scheduler places over
// whatever devices are in the fabric — local CPU and GPUs, and every device of
// every attached node. Nothing numerical happens in this function: it builds
// the forward pass, the backward pass and the optimizer step out of Operations
// and reads the results back. The host does only what has no operation yet:
// the embedding gather (x0 from tok_emb + pos_emb) and its scatter back into
// the two embedding gradients.
// ---------------------------------------------------------------------------
namespace {

struct GraphBuilder {
  MemoryFabric& mem;
  IdGenerator& ids;
  EntityID owner;
  ComputeGraph graph;
  std::vector<MemoryID> owned;
  std::string device_selector;
  std::string error;

  GraphBuilder(MemoryFabric& m, IdGenerator& i, const EntityID& o, std::string selector)
      : mem(m), ids(i), owner(o), device_selector(std::move(selector)) {
    graph.id = ids.next_as<GraphID>();
    graph.owner = owner;
  }
  ~GraphBuilder() { for (auto& m : owned) mem.release(m); }

  Tensor tensor(std::vector<uint64_t> shape, const float* init = nullptr) {
    Tensor t;
    t.dtype = DType::F32;
    t.shape = std::move(shape);
    if (!error.empty()) return t;
    uint64_t bytes = t.bytes();
    auto r = mem.allocate(owner, std::max<uint64_t>(bytes, 4));
    if (!r.ok()) { error = "memory: " + r.error().str(); return t; }
    owned.push_back(*r);
    t.memory = *r;
    if (init) {
      auto w = mem.write(*r, 0, ByteSpan{reinterpret_cast<const uint8_t*>(init), bytes});
      if (!w.ok()) error = "memory write: " + w.error().str();
    } else {
      std::vector<float> zero(t.elements(), 0.0f);
      auto w = mem.write(*r, 0, ByteSpan{reinterpret_cast<const uint8_t*>(zero.data()), bytes});
      if (!w.ok()) error = "memory write: " + w.error().str();
    }
    return t;
  }
  // The same memory seen as a smaller matrix (leading rows).
  static Tensor rows(const Tensor& t, uint64_t n) { Tensor v = t; v.shape[0] = n; return v; }

  Tensor op(OpCode code, std::vector<Tensor> in, std::vector<Tensor> out, double scalar = 0.0) {
    Operation o;
    o.id = ids.next_as<OperationID>();
    o.code = code;
    o.inputs = std::move(in);
    o.outputs = std::move(out);
    o.scalar = scalar;
    o.requirement.device_selector = device_selector;
    for (auto& t : o.inputs) o.requirement.memory += t.bytes();
    for (auto& t : o.outputs) o.requirement.memory += t.bytes();
    o.requirement.parallelism = 64;
    Tensor first = o.outputs.empty() ? Tensor{} : o.outputs[0];
    graph.operations.push_back(std::move(o));
    return first;
  }
  Tensor matmul(const Tensor& a, const Tensor& b) { return op(OpCode::MatMul, {a, b}, {tensor({a.shape[0], b.shape[1]})}); }
  Tensor bias_add(const Tensor& x, const Tensor& b) { return op(OpCode::BiasAdd, {x, b}, {tensor(x.shape)}); }
  Tensor add(const Tensor& a, const Tensor& b) { return op(OpCode::Add, {a, b}, {tensor(a.shape)}); }
  Tensor layernorm(const Tensor& x, const Tensor& g, const Tensor& b) { return op(OpCode::LayerNorm, {x, g, b}, {tensor(x.shape)}); }
  Tensor gelu(const Tensor& x) { return op(OpCode::Gelu, {x}, {tensor(x.shape)}); }
  Tensor bias_grad(const Tensor& dy, uint64_t n) { return op(OpCode::BiasAddGrad, {dy}, {tensor({n})}); }
  Tensor grad_a(const Tensor& dc, const Tensor& b) { return op(OpCode::MatMulGradA, {dc, b}, {tensor({dc.shape[0], b.shape[0]})}); }
  Tensor grad_b(const Tensor& a, const Tensor& dc) { return op(OpCode::MatMulGradB, {a, dc}, {tensor({a.shape[1], dc.shape[1]})}); }

  Result<std::vector<float>> read(const Tensor& t) {
    auto b = mem.read(t.memory, t.offset, t.bytes());
    if (!b.ok()) return b.error();
    std::vector<float> v(t.elements());
    std::memcpy(v.data(), b->data(), std::min<size_t>(b->size(), v.size() * sizeof(float)));
    return v;
  }
};

}  // namespace

float TinyTransformerLM::train_step_graph(const std::vector<std::vector<int>>& batch, float lr,
                                          Scheduler& scheduler, MemoryFabric& memory, IdGenerator& ids,
                                          const EntityID& owner, GraphTrainReport* report,
                                          const std::string& device_selector) {
  if (!ready() || batch.empty()) return last_loss_;
  const int D = cfg_.d_model, H = cfg_.n_head, F = cfg_.d_ff, V = vocab_;
  enum { G_TOK = 0, G_POS, G_GF, G_BF, G_HEAD, G_HEADB, G_LAYER0 };
  enum { L_G1 = 0, L_B1, L_G2, L_B2, L_WQ, L_WK, L_WV, L_WO, L_BQ, L_BK, L_BV, L_BO, L_W1, L_B1F, L_W2, L_B2F };
  auto lslot = [&](std::vector<std::vector<float>>& g, int li, int which) -> std::vector<float>& {
    return g[(size_t)G_LAYER0 + (size_t)li * 16 + (size_t)which];
  };
  auto fail = [&](const std::string& why) {
    if (report) report->failure = why;
    return last_loss_;
  };
  auto note = [&](const ExecutionReport& rep) {
    if (!report) return;
    report->operations += rep.operations_run;
    for (auto& p : rep.placements) report->per_device[p.device_name]++;
    report->elapsed_ns += rep.elapsed_ns;
  };

  std::vector<std::vector<float>> g = zero_gradients();
  float total = 0.0f;
  int sequences = 0;

  for (const auto& seq_in : batch) {
    if (seq_in.size() < 2) continue;
    const int T = (int)std::min((size_t)cfg_.block, seq_in.size());
    if (T < 2) continue;
    std::vector<int> seq(seq_in.begin(), seq_in.begin() + T);
    GraphBuilder b(memory, ids, owner, device_selector);

    // ---- what the host does: the embedding gather
    std::vector<float> x0((size_t)T * D);
    for (int t = 0; t < T; ++t) {
      const int id = std::clamp(seq[(size_t)t], 0, V - 1);
      for (int j = 0; j < D; ++j) x0[(size_t)t * D + j] = tok_emb_[(size_t)id * D + j] + pos_emb_[(size_t)t * D + j];
    }
    std::vector<float> targets((size_t)T - 1);
    for (int t = 0; t + 1 < T; ++t) targets[(size_t)t] = (float)std::clamp(seq[(size_t)t + 1], 0, V - 1);

    Tensor x = b.tensor({(uint64_t)T, (uint64_t)D}, x0.data());
    Tensor tgt = b.tensor({(uint64_t)T - 1}, targets.data());

    // ---- parameters as tensors, in params() order so the gradients land in g
    struct LayerT { Tensor g1, b1, g2, b2, Wq, Wk, Wv, Wo, bq, bk, bv, bo, W1, b1f, W2, b2f; };
    std::vector<LayerT> P((size_t)cfg_.n_layer);
    for (int li = 0; li < cfg_.n_layer; ++li) {
      const Layer& L = layers_[(size_t)li];
      LayerT& t = P[(size_t)li];
      t.g1 = b.tensor({(uint64_t)D}, L.g1.data()); t.b1 = b.tensor({(uint64_t)D}, L.b1.data());
      t.g2 = b.tensor({(uint64_t)D}, L.g2.data()); t.b2 = b.tensor({(uint64_t)D}, L.b2.data());
      t.Wq = b.tensor({(uint64_t)D, (uint64_t)D}, L.Wq.data()); t.Wk = b.tensor({(uint64_t)D, (uint64_t)D}, L.Wk.data());
      t.Wv = b.tensor({(uint64_t)D, (uint64_t)D}, L.Wv.data()); t.Wo = b.tensor({(uint64_t)D, (uint64_t)D}, L.Wo.data());
      t.bq = b.tensor({(uint64_t)D}, L.bq.data()); t.bk = b.tensor({(uint64_t)D}, L.bk.data());
      t.bv = b.tensor({(uint64_t)D}, L.bv.data()); t.bo = b.tensor({(uint64_t)D}, L.bo.data());
      t.W1 = b.tensor({(uint64_t)D, (uint64_t)F}, L.W1.data()); t.b1f = b.tensor({(uint64_t)F}, L.b1f.data());
      t.W2 = b.tensor({(uint64_t)F, (uint64_t)D}, L.W2.data()); t.b2f = b.tensor({(uint64_t)D}, L.b2f.data());
    }
    Tensor gf = b.tensor({(uint64_t)D}, gf_.data()), bf = b.tensor({(uint64_t)D}, bf_.data());
    Tensor head = b.tensor({(uint64_t)D, (uint64_t)V}, head_.data()), head_b = b.tensor({(uint64_t)V}, head_b_.data());
    if (!b.error.empty()) return fail(b.error);

    // ---- forward
    struct Saved { Tensor pre, h1, q, k, v, att, o, mid, h2, ffpre, ff; };
    std::vector<Saved> S((size_t)cfg_.n_layer);
    for (int li = 0; li < cfg_.n_layer; ++li) {
      LayerT& t = P[(size_t)li];
      Saved& s = S[(size_t)li];
      s.pre = x;
      s.h1 = b.layernorm(x, t.g1, t.b1);
      s.q = b.bias_add(b.matmul(s.h1, t.Wq), t.bq);
      s.k = b.bias_add(b.matmul(s.h1, t.Wk), t.bk);
      s.v = b.bias_add(b.matmul(s.h1, t.Wv), t.bv);
      s.o = b.tensor({(uint64_t)T, (uint64_t)D});
      s.att = b.tensor({(uint64_t)H, (uint64_t)T, (uint64_t)T});
      b.op(OpCode::CausalAttention, {s.q, s.k, s.v}, {s.o, s.att}, (double)H);
      s.mid = b.add(x, b.bias_add(b.matmul(s.o, t.Wo), t.bo));
      s.h2 = b.layernorm(s.mid, t.g2, t.b2);
      s.ffpre = b.bias_add(b.matmul(s.h2, t.W1), t.b1f);
      s.ff = b.gelu(s.ffpre);
      x = b.add(s.mid, b.bias_add(b.matmul(s.ff, t.W2), t.b2f));
    }
    Tensor x_last = x;
    Tensor xf = b.layernorm(x_last, gf, bf);
    Tensor logits = b.bias_add(b.matmul(xf, head), head_b);          // [T x V]
    Tensor loss = b.tensor({1});
    Tensor probs = b.tensor({(uint64_t)T - 1, (uint64_t)V});
    b.op(OpCode::SoftmaxCrossEntropyLoss, {GraphBuilder::rows(logits, (uint64_t)T - 1), tgt}, {loss, probs});

    // ---- backward, bottom to top of the forward above
    Tensor dlogits = b.tensor({(uint64_t)T, (uint64_t)V});          // zero-filled: the last row has no target
    b.op(OpCode::SoftmaxCrossEntropyGrad, {probs, tgt}, {GraphBuilder::rows(dlogits, (uint64_t)T - 1)});
    Tensor d_head_b = b.bias_grad(dlogits, (uint64_t)V);
    Tensor d_head = b.grad_b(xf, dlogits);
    Tensor dxf = b.grad_a(dlogits, head);
    Tensor dx = b.tensor({(uint64_t)T, (uint64_t)D}), d_gf = b.tensor({(uint64_t)D}), d_bf = b.tensor({(uint64_t)D});
    b.op(OpCode::LayerNormGrad, {dxf, x_last, gf}, {dx, d_gf, d_bf});

    struct LayerG { Tensor g1, b1, g2, b2, Wq, Wk, Wv, Wo, bq, bk, bv, bo, W1, b1f, W2, b2f; };
    std::vector<LayerG> G((size_t)cfg_.n_layer);
    for (int li = cfg_.n_layer - 1; li >= 0; --li) {
      LayerT& t = P[(size_t)li]; Saved& s = S[(size_t)li]; LayerG& gr = G[(size_t)li];
      // x = mid + (gelu(h2·W1 + b1f)·W2 + b2f)
      gr.b2f = b.bias_grad(dx, (uint64_t)D);
      gr.W2 = b.grad_b(s.ff, dx);
      Tensor dff = b.grad_a(dx, t.W2);                                                // [T x F]
      Tensor dffpre = b.op(OpCode::GeluGrad, {dff, s.ffpre}, {b.tensor({(uint64_t)T, (uint64_t)F})});
      gr.b1f = b.bias_grad(dffpre, (uint64_t)F);
      gr.W1 = b.grad_b(s.h2, dffpre);
      Tensor dh2 = b.grad_a(dffpre, t.W1);                                            // [T x D]
      Tensor dmid_ln = b.tensor({(uint64_t)T, (uint64_t)D});
      gr.g2 = b.tensor({(uint64_t)D}); gr.b2 = b.tensor({(uint64_t)D});
      b.op(OpCode::LayerNormGrad, {dh2, s.mid, t.g2}, {dmid_ln, gr.g2, gr.b2});
      Tensor dmid = b.add(dx, dmid_ln);
      // mid = pre + (o·Wo + bo)
      gr.bo = b.bias_grad(dmid, (uint64_t)D);
      gr.Wo = b.grad_b(s.o, dmid);
      Tensor dO = b.grad_a(dmid, t.Wo);
      Tensor dq = b.tensor({(uint64_t)T, (uint64_t)D}), dk = b.tensor({(uint64_t)T, (uint64_t)D}), dv = b.tensor({(uint64_t)T, (uint64_t)D});
      b.op(OpCode::CausalAttentionGrad, {dO, s.q, s.k, s.v, s.att}, {dq, dk, dv}, (double)H);
      gr.bq = b.bias_grad(dq, (uint64_t)D); gr.bk = b.bias_grad(dk, (uint64_t)D); gr.bv = b.bias_grad(dv, (uint64_t)D);
      gr.Wq = b.grad_b(s.h1, dq); gr.Wk = b.grad_b(s.h1, dk); gr.Wv = b.grad_b(s.h1, dv);
      Tensor dh1 = b.add(b.add(b.grad_a(dq, t.Wq), b.grad_a(dk, t.Wk)), b.grad_a(dv, t.Wv));
      Tensor dpre_ln = b.tensor({(uint64_t)T, (uint64_t)D});
      gr.g1 = b.tensor({(uint64_t)D}); gr.b1 = b.tensor({(uint64_t)D});
      b.op(OpCode::LayerNormGrad, {dh1, s.pre, t.g1}, {dpre_ln, gr.g1, gr.b1});
      dx = b.add(dmid, dpre_ln);
    }
    if (!b.error.empty()) return fail(b.error);

    // ---- run it
    SchedulePolicy policy{};
    auto rep = scheduler.execute(b.graph, owner, 0, policy);
    if (!rep.ok()) return fail("scheduler: " + rep.error().str());
    if (!rep->failure.empty()) return fail(rep->failure);
    if (!rep->complete && rep->position < b.graph.operations.size()) return fail("the scheduler stopped the graph at operation " + std::to_string(rep->position) + " of " + std::to_string(b.graph.operations.size()) + (rep->preempted ? " (preempted)" : ""));
    note(*rep);

    // ---- read the results home
    auto acc = [&](std::vector<float>& into, const Tensor& t) -> bool {
      auto v = b.read(t);
      if (!v.ok() || v->size() != into.size()) return false;
      for (size_t i = 0; i < into.size(); ++i) into[i] += (*v)[i];
      return true;
    };
    auto l = b.read(loss);
    if (!l.ok() || l->empty()) return fail("could not read the loss back");
    bool ok = acc(g[G_GF], d_gf) && acc(g[G_BF], d_bf) && acc(g[G_HEAD], d_head) && acc(g[G_HEADB], d_head_b);
    for (int li = 0; li < cfg_.n_layer && ok; ++li) {
      LayerG& gr = G[(size_t)li];
      const Tensor* slots[16] = {&gr.g1, &gr.b1, &gr.g2, &gr.b2, &gr.Wq, &gr.Wk, &gr.Wv, &gr.Wo, &gr.bq, &gr.bk, &gr.bv, &gr.bo, &gr.W1, &gr.b1f, &gr.W2, &gr.b2f};
      for (int w = 0; w < 16 && ok; ++w) ok = acc(lslot(g, li, w), *slots[w]);
    }
    if (!ok) return fail("could not read a gradient back from fabric memory");
    // The embedding scatter, the other half of what the host owns.
    auto dx0 = b.read(dx);
    if (!dx0.ok() || dx0->size() != (size_t)T * D) return fail("could not read the embedding gradient back");
    for (int t = 0; t < T; ++t) {
      const int id = std::clamp(seq[(size_t)t], 0, V - 1);
      for (int j = 0; j < D; ++j) {
        const float d = (*dx0)[(size_t)t * D + j];
        g[G_TOK][(size_t)id * D + j] += d;
        g[G_POS][(size_t)t * D + j] += d;
      }
    }
    total += (*l)[0];
    ++sequences;
  }
  if (!sequences) return last_loss_;
  const float inv_batch = 1.0f / (float)sequences;
  // The step and its loss are committed only once the optimizer graph has run
  // and its results are read home: a step whose update never landed is not a step.
  const float batch_loss = total * inv_batch;
  const uint64_t next_step = steps_ + 1;

  // ---- AdamW on the graph: one adam-step per parameter tensor, all in one
  // graph so the scheduler can spread them. The op has one scalar, so the
  // bias correction rides in it: lr_t = lr * sqrt(1-b2^t) / (1-b1^t) is the
  // efficient form from the Adam paper (§2), identical to correcting m and v.
  const float b1 = 0.9f, b2 = 0.999f;
  const float c1 = 1.0f - std::pow(b1, (float)next_step);
  const float c2 = 1.0f - std::pow(b2, (float)next_step);
  const float lr_t = lr * std::sqrt(c2) / c1;
  {
    GraphBuilder b(memory, ids, owner, device_selector);
    std::vector<float*> ps = params();
    struct Slot { Tensor w, g, m, v; float* wp; size_t at; uint64_t n; };
    std::vector<Slot> slots;
    size_t at = 0;
    for (size_t pi = 0; pi < ps.size(); ++pi) {
      uint64_t n = g[pi].size();
      if (!n) continue;
      std::vector<float> scaled(n);
      for (uint64_t i = 0; i < n; ++i) scaled[i] = g[pi][i] * inv_batch;
      Slot s;
      s.w = b.tensor({n}, ps[pi]); s.g = b.tensor({n}, scaled.data());
      s.m = b.tensor({n}, &m_[at]); s.v = b.tensor({n}, &v_[at]);
      s.wp = ps[pi]; s.at = at; s.n = n;
      b.op(OpCode::AdamStep, {s.w, s.g, s.m, s.v}, {s.w}, (double)lr_t);
      slots.push_back(s);
      at += n;
    }
    if (!b.error.empty()) return fail(b.error);
    SchedulePolicy policy{};
    auto rep = scheduler.execute(b.graph, owner, 0, policy);
    if (!rep.ok()) return fail("scheduler: " + rep.error().str());
    if (!rep->failure.empty()) return fail(rep->failure);
    note(*rep);
    for (auto& s : slots) {
      auto w = b.read(s.w); auto m = b.read(s.m); auto v = b.read(s.v);
      if (!w.ok() || !m.ok() || !v.ok() || w->size() != s.n || m->size() != s.n || v->size() != s.n) return fail("could not read the updated weights back");
      std::memcpy(s.wp, w->data(), s.n * sizeof(float));
      std::memcpy(&m_[s.at], m->data(), s.n * sizeof(float));
      std::memcpy(&v_[s.at], v->data(), s.n * sizeof(float));
    }
  }
  steps_ = next_step;
  last_loss_ = batch_loss;
  return last_loss_;
}

std::vector<int> TinyTransformerLM::generate(const std::vector<int>& context_ids, int max_new,
                                             const std::vector<int>& stop_ids, uint32_t seed) const {
  std::vector<int> out;
  if (!ready() || max_new <= 0) return out;
  std::vector<int> ctx = context_ids;
  Rng rng(seed);
  for (int n = 0; n < max_new; ++n) {
    if (ctx.empty()) break;
    // The window is the last `block` tokens: the position embedding has no
    // meaning past it, so feeding more would be reading a table off its end.
    std::vector<int> window;
    const size_t start = ctx.size() > (size_t)cfg_.block ? ctx.size() - (size_t)cfg_.block : 0;
    window.assign(ctx.begin() + (long)start, ctx.end());
    Cache c;
    forward(window, c);
    const int t = c.T - 1;
    // seed 0: the mode (greedy). Any other seed: a sample from the model's own
    // distribution, drawn with that seed, so the same seed gives the same text.
    int best = 0;
    if (seed == 0) {
      float bp = -1.0f;
      for (int w = 0; w < vocab_; ++w) {
        const float p = c.probs[(size_t)t * vocab_ + w];
        if (p > bp) { bp = p; best = w; }
      }
    } else {
      float r = rng.uniform(), acc = 0.0f;
      best = vocab_ - 1;
      for (int w = 0; w < vocab_; ++w) {
        acc += c.probs[(size_t)t * vocab_ + w];
        if (r < acc) { best = w; break; }
      }
    }
    // A stop token ends the reply. It is kept when it carries text before its
    // newline (BPE merges "\n" into longer tokens); the caller cuts at the '\n'.
    if (std::find(stop_ids.begin(), stop_ids.end(), best) != stop_ids.end()) {
      out.push_back(best);
      break;
    }
    out.push_back(best);
    ctx.push_back(best);
  }
  return out;
}

void TinyTransformerLM::serialize(std::string& out) const {
  // "TLM2": TLM1 plus the AdamW moments, so a resumed run continues the same
  // optimisation instead of restarting the moments from zero.
  put_u32(out, 0x544C4D32u);
  put_u32(out, (uint32_t)vocab_);
  put_u32(out, (uint32_t)cfg_.d_model);
  put_u32(out, (uint32_t)cfg_.n_layer);
  put_u32(out, (uint32_t)cfg_.n_head);
  put_u32(out, (uint32_t)cfg_.d_ff);
  put_u32(out, (uint32_t)cfg_.block);
  put_u64(out, steps_);
  put_f32(out, last_loss_);
  std::vector<const float*> ps = params();
  const std::vector<size_t> sizes = param_sizes();
  for (size_t i = 0; i < ps.size(); ++i) {
    for (size_t j = 0; j < sizes[i]; ++j) put_f32(out, ps[i][j]);
  }
  put_u64(out, (uint64_t)m_.size());
  for (float f : m_) put_f32(out, f);
  for (float f : v_) put_f32(out, f);
}

bool TinyTransformerLM::deserialize(const std::string& blob, size_t& off) {
  uint32_t magic = 0, v = 0, d = 0, nl = 0, nh = 0, ff = 0, blk = 0;
  if (!get_u32(blob, off, magic) || (magic != 0x544C4D31u && magic != 0x544C4D32u)) return false;
  if (!get_u32(blob, off, v) || !get_u32(blob, off, d) || !get_u32(blob, off, nl) ||
      !get_u32(blob, off, nh) || !get_u32(blob, off, ff) || !get_u32(blob, off, blk)) return false;
  TinyLmConfig cfg;
  cfg.d_model = (int)d; cfg.n_layer = (int)nl; cfg.n_head = (int)nh; cfg.d_ff = (int)ff; cfg.block = (int)blk;
  if (cfg.d_model <= 0 || cfg.n_head <= 0 || cfg.d_model % cfg.n_head != 0) return false;
  init((int)v, cfg);
  uint64_t steps = 0;
  float loss = 0.0f;
  if (!get_u64(blob, off, steps) || !get_f32(blob, off, loss)) return false;
  std::vector<float*> ps = params();
  const std::vector<size_t> sizes = param_sizes();
  for (size_t i = 0; i < ps.size(); ++i) {
    for (size_t j = 0; j < sizes[i]; ++j) {
      if (!get_f32(blob, off, ps[i][j])) return false;
    }
  }
  if (magic == 0x544C4D32u) {
    uint64_t n = 0;
    if (!get_u64(blob, off, n) || n != m_.size()) return false;
    for (auto& f : m_) if (!get_f32(blob, off, f)) return false;
    for (auto& f : v_) if (!get_f32(blob, off, f)) return false;
  }
  // A TLM1 checkpoint has no moments: they stay at zero, and the resumed run's
  // first steps re-estimate them.
  steps_ = steps;
  last_loss_ = loss;
  return true;
}


// ---------------------------------------------------------------------------
// The resident model: its corpus, its training, and the one thing it does.
// ---------------------------------------------------------------------------
namespace {

// What this model is trained to recognise. Each line is a request a person
// might type and the console command it means. This is the machine's corpus
// about itself; the model learns the mapping from it, and `answer` scores every
// command in the table by likelihood rather than matching a keyword.
struct Pair { const char* say; const char* cmd; };
const Pair kPairs[] = {
  {"what are you", "identity"},
  {"who are you", "identity"},
  {"what is this machine", "identity"},
  {"tell me about yourself", "identity"},
  {"what do you hold", "info"},
  {"how are you doing", "info"},
  {"show me the system", "info"},
  {"what is your state", "info"},
  {"give me an overview", "info"},
  {"what hardware do you have", "devices"},
  {"list the devices", "devices"},
  {"what can you compute on", "devices"},
  {"show me the accelerators", "devices"},
  {"what may each entity do", "authority"},
  {"show me the capabilities", "authority"},
  {"who is allowed to do what", "authority"},
  {"what authority do you hold", "authority"},
  {"what decisions did you make about authority", "authority log"},
  {"show me the ledger", "authority log"},
  {"what did you refuse", "authority log"},
  {"list the entities", "entity list"},
  {"who lives here", "entity list"},
  {"what entities are active", "entity list"},
  {"what is on the horizon", "horizon"},
  {"what did the world do", "horizon"},
  {"show me the signals", "horizon"},
  {"what does it mean", "horizon brief"},
  {"read me the brief", "horizon brief"},
  {"what storage do you have", "storage"},
  {"show me the volumes", "storage"},
  {"what is your network", "network"},
  {"show me the interfaces", "network"},
  {"how is the mind bound", "mind status"},
  {"what model are you using", "mind status"},
  {"what is your world model doing", "jepa status"},
  {"show me the predictor", "jepa status"},
  {"what work runs by itself", "directive list"},
  {"list the standing orders", "directive list"},
  {"what services are running", "service list"},
  {"show me the daemons", "service list"},
  {"how much memory is in use", "sysstat"},
  {"show me the statistics", "sysstat"},
  {"what is the load", "sysstat"},
  {"who am i", "whoami"},
  {"what am i talking to", "whoami"},
  {"list the objects you remember", "store list"},
  {"what do you remember", "store list"},
  {"take a checkpoint", "checkpoint console"},
  {"save your state", "checkpoint console"},
  {"list the subsystems", "subsystems"},
  {"what is built and what is not", "subsystems"},
  // The display, asked for by name. These are the requests a person actually
  // makes of a machine with a screen, and they reach a lever whose effect can
  // be looked at rather than believed.
  {"show me a pyramid", "display show pyramid"},
  {"draw me a pyramid", "display show pyramid"},
  {"put a pyramid on the screen", "display show pyramid"},
  {"i want to see a pyramid", "display show pyramid"},
  {"render a pyramid", "display show pyramid"},
  {"show me the desert", "display show pyramid"},
  {"show me the ocean", "display show ocean"},
  {"show me the sea", "display show ocean"},
  {"draw me the ocean", "display show ocean"},
  {"show me the ocean at dusk", "display show ocean at dusk"},
  {"show me the ocean at sunset", "display show ocean at dusk"},
  {"ocean at dusk", "display show ocean at dusk"},
  {"ocean at sunset", "display show ocean at dusk"},
  {"draw the ocean at dusk", "display show ocean at dusk"},
  {"three pyramids in the desert", "display show three pyramids in the desert"},
  {"i want to see the ocean", "display show ocean"},
  {"put the ocean on the screen", "display show ocean"},
  {"show me the moon", "display show ocean"},
  {"show me the lattice", "display show cyber"},
  {"show me the matrix", "display show cyber"},
  {"render the lattice", "display show cyber"},
  {"show me the orb", "display show orb"},
  {"show me the plasma core", "display show orb"},
  {"render the core", "display show orb"},
  {"show me the desk", "display desk"},
  {"bring back the desk", "display desk"},
  {"show me the ledger again", "display desk"},
  {"what is on the display", "display status"},
  {"what are you showing", "display status"},
  {"what can you draw", "display status"},
  // The network and the load, measured and then corrected.
  {"check the network", "net check"},
  {"are we losing packets", "net check"},
  {"what is the packet loss", "net check"},
  {"fix the network", "net repair"},
  {"correct the packet loss", "net repair"},
  {"repair the network", "net repair"},
  {"optimize the load", "host load correct"},
  {"correct the load", "host load correct"},
  {"the machine is too busy", "host load correct"},
  {"optimize a process", "host load correct"},
};
constexpr size_t kPairCount = sizeof(kPairs) / sizeof(kPairs[0]);

// The separator the model is trained to emit between a request and a command.
// It has to be a sequence that cannot occur inside either, so the boundary is
// unambiguous both in training and when a continuation is being read back.
const char* const kArrow = " >>> ";

}  // namespace

void MachineLanguageModel::build_corpus() {
  corpus_.clear();
  commands_.clear();
  for (size_t i = 0; i < kPairCount; ++i) {
    corpus_.push_back(std::string(kPairs[i].say) + kArrow + kPairs[i].cmd + "\n");
    bool have = false;
    for (const auto& c : commands_) if (c == kPairs[i].cmd) { have = true; break; }
    if (!have) commands_.push_back(kPairs[i].cmd);
  }
  // A machine that can only map requests onto commands has learned a lookup
  // table. These lines are what it knows ABOUT itself, in its own words, so the
  // vocabulary carries the substrate's terms and not only the imperative ones.
  static const char* const kFacts[] = {
    "this machine is an operating system in which intelligence is one class of thing the kernel supports\n",
    "every entity here holds capabilities and the kernel refuses anything it was not granted\n",
    "a capability can be delegated, attenuated, and revoked, and revoking cascades\n",
    "an entity can be checkpointed mid execution and put back with the same identity\n",
    "the kernel keeps residents across boots and records every decision about authority\n",
    "memory objects keep their identity when they migrate between system, shared and persistent\n",
    "a compute graph is scheduled in dependency waves and can resume from its execution position\n",
    "the mind is replaceable and the kernel has no knowledge of which one is bound\n",
    "the state vector is measured from this machine's own counters, not synthesised\n",
    "the policy head sets the scheduler quantum and is trained on a measured reward\n",
    "the policy head can also choose what goes on this machine's own display\n",
    "the display can draw a tide under a moon, a lattice, a plasma core and a pyramid\n",
    "packet loss is measured from the interfaces' own dropped and errored counters\n",
    "the refill window is how many receive buffers the driver keeps posted\n",
    "load imbalance is runnable work over the processors that could run it\n",
    "a quota is the share of measured device time an entity may use before it waits\n",
  };
  for (const char* f : kFacts) corpus_.push_back(f);
}

std::string MachineLanguageModel::state_sentence() const {
  const SubstrateState s = SubstrateState::sample_kernel_telemetry();
  if (s.vector.size() < 6) return "the state vector is not available";
  auto pct = [](float v) {
    int p = (int)(v * 100.0f + 0.5f);
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    return std::to_string(p);
  };
  const bool measured = s.vector[5] > 0.5f;
  std::string out = measured ? "measured now: " : "SYNTHETIC (no telemetry provider registered): ";
  out += "cpu load " + pct(s.vector[0]) + "%";
  out += ", context switches " + pct(s.vector[2]) + "%";
  out += ", heap pressure " + pct(s.vector[3]) + "%";
  out += ", nic depth " + pct(s.vector[4]) + "%";
  return out;
}

bool MachineLanguageModel::ensure_ready(std::string* what_happened) {
  if (trained_) return true;
  build_corpus();

  // A fingerprint of the corpus the weights were trained on. Without it, adding
  // a line to the corpus left the machine loading weights whose VOCABULARY does
  // not contain the new words — it would answer, confidently, from a model that
  // had never seen the request. Weights trained on different text are stale
  // weights, and stale weights are retrained rather than trusted.
  uint64_t fp = 0xcbf29ce484222325ull;
  for (const auto& line : corpus_) {
    for (unsigned char ch : line) { fp ^= ch; fp *= 0x100000001b3ull; }
  }
  corpus_fingerprint_ = fp;

  // Persisted weights first: training is expensive and a machine that retrains
  // on every question is a machine nobody waits for.
  if (g_read) {
    std::string blob;
    if (g_read("language-model", blob)) {
      size_t off = 0;
      uint32_t lo = 0, hi = 0;
      const bool tagged = get_u32(blob, off, lo) && get_u32(blob, off, hi);
      const uint64_t stored = (uint64_t)lo | ((uint64_t)hi << 32);
      if (tagged && stored != fp) {
        note_ = "the stored weights were trained on a different corpus; retraining";
        off = blob.size();   // fall through to training below
      } else if (tagged && tok_.deserialize(blob, off) && lm_.deserialize(blob, off)) {
        trained_ = true;
        note_ = "loaded " + std::to_string(lm_.parameter_count()) + " parameters trained for " +
                std::to_string(lm_.train_steps()) + " steps (loss " + std::to_string(lm_.last_loss()) + ")";
        if (what_happened) *what_happened = note_;
        return true;
      }
    }
  }

  // Nothing stored: learn the vocabulary and take a first pass over the corpus.
  // The step count here is deliberately modest — this runs inside a question a
  // person is waiting on, and `lm train` is how it is taken further.
  tok_.train(corpus_, 512);
  TinyLmConfig cfg;
  lm_.init((int)tok_.vocab_size(), cfg);
  std::vector<std::vector<int>> encoded;
  encoded.reserve(corpus_.size());
  for (const auto& line : corpus_) {
    auto ids = tok_.encode(line);
    if (ids.size() >= 2) encoded.push_back(std::move(ids));
  }
  if (encoded.empty()) {
    note_ = "no corpus: the model has nothing to be trained on";
    if (what_happened) *what_happened = note_;
    return false;
  }
  float loss = 0.0f;
  size_t at = 0;
  for (int step = 0; step < 60; ++step) {
    std::vector<std::vector<int>> batch;
    for (int b = 0; b < 4; ++b) { batch.push_back(encoded[at % encoded.size()]); ++at; }
    loss = lm_.train_step(batch, 3e-3f);
  }
  trained_ = true;
  note_ = "trained here: " + std::to_string(tok_.vocab_size()) + " token vocabulary, " +
          std::to_string(lm_.parameter_count()) + " parameters, " +
          std::to_string(lm_.train_steps()) + " steps, loss " + std::to_string(loss) +
          (lm_store_is_live() ? "" : " (no store registered: this is gone at exit)");
  if (g_write) {
    std::string blob;
    put_u32(blob, (uint32_t)(corpus_fingerprint_ & 0xFFFFFFFFu));
    put_u32(blob, (uint32_t)(corpus_fingerprint_ >> 32));
    tok_.serialize(blob);
    lm_.serialize(blob);
    g_write("language-model", blob);
  }
  if (what_happened) *what_happened = note_;
  return true;
}

LanguageReply MachineLanguageModel::answer(const std::string& request) {
  LanguageReply reply;
  reply.explanation = state_sentence();
  if (!ensure_ready()) {
    reply.command = keyword_fallback_command(request);
    reply.used_model = false;
    return reply;
  }

  // The model chooses by LIKELIHOOD, not by matching: for every command it was
  // trained on, score the sequence "<request> >>> <command>" under the model
  // and take the one it finds least surprising. That is a real use of a language
  // model as a classifier, and the number it chose by is reported.
  float best = 1e30f;
  for (const auto& cmd : commands_) {
    const std::string probe = request + kArrow + cmd + "\n";
    const auto ids = tok_.encode(probe);
    if (ids.size() < 2) continue;
    const float nll = lm_.sequence_loss(ids);
    if (nll < best) { best = nll; reply.command = cmd; }
  }
  reply.command_nll = best;
  reply.used_model = !reply.command.empty();

  // And what it actually generates, verbatim, so a person can see the model
  // rather than only its decision. Greedy, cut at the newline it was trained
  // to end a command with.
  const auto ctx = tok_.encode(request + kArrow);
  // Every token whose bytes contain a newline ends the reply: the tokenizer
  // merges "\n" into longer tokens, so the bare "\n" token alone never fired.
  std::vector<int> stops;
  for (int id = 0; id < (int)tok_.vocab_size(); ++id)
    if (tok_.decode({id}).find('\n') != std::string::npos) stops.push_back(id);
  const auto out = lm_.generate(ctx, 24, stops, 0u);   // seed 0: greedy
  reply.generated = tok_.decode(out);
  const size_t cut = reply.generated.find('\n');
  if (cut != std::string::npos) reply.generated.resize(cut);

  if (reply.command.empty()) {
    reply.command = keyword_fallback_command(request);
    reply.used_model = false;
  }
  return reply;
}

MachineLanguageModel* get_machine_language_model() {
  static MachineLanguageModel model;
  return &model;
}

std::string keyword_fallback_command(const std::string& input) {
  std::string lower;
  lower.reserve(input.size());
  for (char c : input) lower += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  auto has = [&](const char* w) { return lower.find(w) != std::string::npos; };
  if (has("who are you") || has("what are you") || has("identity")) return "identity";
  if (has("capab") || has("allowed") || has("authority")) return "authority";
  if (has("refus") || has("ledger") || has("decision")) return "authority log";
  if (has("device") || has("hardware") || has("gpu")) return "devices";
  if (has("entit")) return "entity list";
  if (has("horizon") || has("world")) return "horizon";
  if (has("brief") || has("mean")) return "horizon brief";
  if (has("storage") || has("disk") || has("volume")) return "storage";
  if (has("network") || has("interface")) return "network";
  if (has("mind") || has("model")) return "mind status";
  if (has("predict") || has("jepa")) return "jepa status";
  if (has("directive") || has("standing")) return "directive list";
  if (has("service") || has("daemon")) return "service list";
  if (has("memory") || has("load") || has("stat")) return "sysstat";
  if (has("remember") || has("object")) return "store list";
  return "info";
}

LanguageReply translate_request(const std::string& input) {
  MachineLanguageModel* m = get_machine_language_model();
  LanguageReply r = m->answer(input);
  if (r.command.empty()) {
    r.command = keyword_fallback_command(input);
    r.used_model = false;
  }
  return r;
}


const std::vector<std::string>& machine_corpus() {
  MachineLanguageModel* m = get_machine_language_model();
  m->ensure_ready();
  return m->corpus();
}

void persist_machine_language_model() {
  if (!g_write) return;
  MachineLanguageModel* m = get_machine_language_model();
  std::string blob;
  const uint64_t fp = m->corpus_fingerprint();
  put_u32(blob, (uint32_t)(fp & 0xFFFFFFFFu));
  put_u32(blob, (uint32_t)(fp >> 32));
  m->tokenizer().serialize(blob);
  m->model().serialize(blob);
  g_write("language-model", blob);
}

// ---------------------------------------------------------------------------
// The backward pass, checked against the function it differentiates.
// ---------------------------------------------------------------------------
GradientCheck gradient_check(TinyTransformerLM& lm, const std::vector<int>& ids, int samples, int sabotage_slot) {
  GradientCheck out;
  if (!lm.ready() || ids.size() < 3) return out;
  if (samples < 1) samples = 1;

  std::vector<std::vector<float>> g = lm.zero_gradients();
  lm.accumulate_gradients(ids, g);
  // A deliberately wrong backward pass, to show the check can fail: one
  // parameter tensor's analytic gradient is negated.
  if (sabotage_slot >= 0 && (size_t)sabotage_slot < g.size()) {
    for (auto& v : g[(size_t)sabotage_slot]) v = -v;
    out.sabotaged = sabotage_slot;
  }

  // Names in the same order params() lays them out, so a failing entry says
  // WHICH derivative is wrong rather than only that one is.
  std::vector<std::string> names{"tok_emb", "pos_emb", "final_ln_g", "final_ln_b", "head", "head_b"};
  for (int li = 0; li < lm.config().n_layer; ++li) {
    const std::string p = "L" + std::to_string(li) + ".";
    const char* which[] = {"ln1_g", "ln1_b", "ln2_g", "ln2_b", "Wq", "Wk", "Wv", "Wo",
                           "bq", "bk", "bv", "bo", "W1", "b1", "W2", "b2"};
    for (const char* w : which) names.push_back(p + w);
  }

  // A spread of parameters rather than the first few, which would only ever
  // check the embedding table.
  std::vector<float*> ps = lm.params_for_check();
  const std::vector<size_t> sizes = lm.param_sizes();
  Rng rng(0xC0FFEEu);
  const float h = 1e-3f;

  // What a central difference can resolve here at all. The loss is a float32,
  // so the smallest difference it can represent near L is about eps*|L|, and
  // dividing that by 2h is the noise floor of every number below. A gradient
  // under it is not wrong, it is unmeasurable this way — which is what the
  // first version of this check reported as failure.
  const float L0 = lm.sequence_loss(ids);
  out.floor = 8.0f * 1.1920929e-7f * std::max(1.0f, std::fabs(L0)) / (2.0f * h);

  for (int n = 0; n < samples; ++n) {
    // Pick a slot that is not empty, then an index inside it.
    size_t slot = 0;
    for (int tries = 0; tries < 64; ++tries) {
      slot = (size_t)(rng.next() % (uint32_t)sizes.size());
      if (sizes[slot] > 0) break;
    }
    if (sizes[slot] == 0) continue;
    const size_t at = (size_t)(rng.next() % (uint32_t)sizes[slot]);
    float* w = ps[slot] + at;
    const float saved = *w;

    *w = saved + h;
    const float up = lm.sequence_loss(ids);
    *w = saved - h;
    const float down = lm.sequence_loss(ids);
    *w = saved;

    const float numeric = (up - down) / (2.0f * h);
    const float analytic = g[slot][at];
    GradientCheck::Entry e;
    e.what = (slot < names.size() ? names[slot] : std::string("param")) + "[" + std::to_string(at) + "]";
    e.analytic = analytic;
    e.numeric = numeric;
    const float mag = std::max(std::fabs(numeric), std::fabs(analytic));
    // Four times the floor, not one: at the floor itself the difference IS the
    // noise, and reporting a relative error against noise is reporting noise.
    if (mag < 4.0f * out.floor) { ++out.skipped; continue; }
    e.rel_err = std::fabs(numeric - analytic) / mag;
    if (e.rel_err > out.worst) out.worst = e.rel_err;
    out.entries.push_back(std::move(e));
  }

  // ---- the directional derivative, over every parameter at once
  //
  // This is the test that decides. One parameter moved by h changes a float32
  // loss by about as much as the float32 can represent, so any single-parameter
  // difference is mostly noise — the numbers above are printed to be read, not
  // to be a verdict. Moved ALL together along a random direction, the change in
  // the loss is a sum of a hundred thousand terms and stands well clear of the
  // noise, and it is wrong if ANY part of the backward pass is wrong. Three
  // directions, so agreement cannot be a coincidence of one.
  size_t total = 0;
  for (size_t sz : sizes) total += sz;
  const float hd = 1e-2f;
  for (int d = 0; d < 3; ++d) {
    std::vector<float> dir;
    dir.reserve(total);
    double norm2 = 0.0;
    for (size_t i = 0; i < total; ++i) { const float u = rng.normal(); dir.push_back(u); norm2 += (double)u * u; }
    const float inv = norm2 > 0.0 ? (float)(1.0 / std::sqrt(norm2)) : 1.0f;
    for (auto& u : dir) u *= inv;

    double analytic_dot = 0.0;
    {
      size_t at2 = 0;
      for (size_t si = 0; si < sizes.size(); ++si) {
        for (size_t i = 0; i < sizes[si]; ++i, ++at2) analytic_dot += (double)g[si][i] * dir[at2];
      }
    }
    auto shift = [&](float mul) {
      size_t at2 = 0;
      for (size_t si = 0; si < sizes.size(); ++si) {
        for (size_t i = 0; i < sizes[si]; ++i, ++at2) ps[si][i] += mul * hd * dir[at2];
      }
    };
    // The numeric derivative at step hd and at hd/2. Their disagreement is a
    // measurement of how uncertain the numeric side is (float32 rounding plus
    // truncation), and that uncertainty is what the analytic one is allowed to
    // differ by. A wrong backward pass is caught because both numeric values
    // agree with each other and not with it.
    auto numeric_at = [&](float step) {
      auto move = [&](float mul) {
        size_t at2 = 0;
        for (size_t si = 0; si < sizes.size(); ++si)
          for (size_t i = 0; i < sizes[si]; ++i, ++at2) ps[si][i] += mul * step * dir[at2];
      };
      move(+1.0f);
      const float up_d = lm.sequence_loss(ids);
      move(-2.0f);
      const float down_d = lm.sequence_loss(ids);
      move(+1.0f);   // back where it started (up to float rounding)
      return (up_d - down_d) / (2.0f * step);
    };
    (void)shift;
    GradientCheck::Direction rec;
    rec.numeric = numeric_at(hd);
    rec.numeric_half = numeric_at(hd * 0.5f);
    rec.analytic = (float)analytic_dot;
    const float dmag = std::max(std::fabs(rec.numeric), std::fabs(rec.analytic));
    rec.rel_err = dmag > 1e-9f ? std::fabs(rec.numeric - rec.analytic) / dmag : 0.0f;
    // Absolute floor: float32 resolution of the loss over the step, x4.
    const float noise = 4.0f * 8.0f * 1.1920929e-7f * std::max(1.0f, std::fabs(L0)) / (2.0f * hd * 0.5f);
    rec.tolerance = 0.02f * dmag + noise + 2.0f * std::fabs(rec.numeric - rec.numeric_half);
    rec.agrees = std::fabs(rec.numeric - rec.analytic) <= rec.tolerance;
    if (rec.rel_err > out.dir_worst) out.dir_worst = rec.rel_err;
    out.directions.push_back(rec);
  }
  out.dir_pass = !out.directions.empty();
  for (const auto& d : out.directions) out.dir_pass = out.dir_pass && d.agrees;
  return out;
}

}  // namespace aios::intelligence
