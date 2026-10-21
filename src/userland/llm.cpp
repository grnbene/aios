#include "aios/userland/llm.hpp"
#include "aios/channel/rpc_value.hpp"
#include "aios/hal/lane_pool.hpp"
#include "aios/hal/sync.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>

// The dot products dominate a token's cost; on x86-64 with GCC the kernels are
// also built for AVX2 with FMA, and kernels() picks per machine at run time.
#if defined(__GNUC__) && !defined(__clang__) && defined(__x86_64__) && defined(__linux__)
#define AIOS_LLM_AVX2 1
#else
#define AIOS_LLM_AVX2 0
#endif

namespace aios::userland {
namespace {

using channel::RpcValue;

inline float bf16_to_f32(uint16_t b) {
  const uint32_t u = (uint32_t)b << 16;
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
inline uint16_t f32_to_bf16(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  if ((u & 0x7FFFFFFFu) > 0x7F800000u) return (uint16_t)((u >> 16) | 0x40);   // NaN stays NaN
  u += 0x7FFFu + ((u >> 16) & 1);                                            // round to nearest even
  return (uint16_t)(u >> 16);
}
inline float f16_to_f32(uint16_t h) {
  const uint32_t sign = (uint32_t)(h & 0x8000) << 16;
  uint32_t exp = (h >> 10) & 0x1F, man = h & 0x3FF, u;
  if (exp == 0) {
    if (man == 0) u = sign;
    else {
      exp = 127 - 15 + 1;
      while (!(man & 0x400)) { man <<= 1; --exp; }
      u = sign | (exp << 23) | ((man & 0x3FF) << 13);
    }
  } else if (exp == 31) u = sign | 0x7F800000u | (man << 13);
  else u = sign | ((exp + 127 - 15) << 23) | (man << 13);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// ---- safetensors ---------------------------------------------------------

struct TensorInfo {
  std::string file, dtype;
  std::vector<uint64_t> shape;
  uint64_t offset = 0, bytes = 0;   // absolute offset in the file
  uint64_t elements() const { uint64_t n = 1; for (auto d : shape) n *= d; return n; }
};

class Safetensors {
 public:
  Result<void> open_dir(hal::Host& host, const std::string& dir) {
    host_ = &host;
    auto names = host.list_dir(dir);
    if (!names.ok()) return names.error();
    std::vector<std::string> files;
    for (const auto& n : *names)
      if (n.size() > 12 && n.compare(n.size() - 12, 12, ".safetensors") == 0) files.push_back(n);
    std::sort(files.begin(), files.end());
    if (files.empty()) return Result<void>(Status::NotFound, "no .safetensors file in " + dir);
    for (const auto& f : files) {
      const std::string path = dir + "/" + f;
      auto head = host.read_range(path, 0, 8);
      if (!head.ok()) return head.error();
      if (head->size() < 8) return Result<void>(Status::Invalid, path + " is too short to be safetensors");
      uint64_t hlen = 0;
      for (int i = 7; i >= 0; --i) hlen = (hlen << 8) | (*head)[(size_t)i];
      if (hlen == 0 || hlen > (256u << 20)) return Result<void>(Status::Invalid, path + ": implausible header length");
      auto hdr = host.read_range(path, 8, hlen);
      if (!hdr.ok()) return hdr.error();
      auto doc = channel::rpc_parse(ByteSpan(hdr->data(), hdr->size()));
      if (!doc.ok()) return Result<void>(Status::Invalid, path + ": " + doc.error().message);
      for (const auto& [name, t] : doc->fields()) {
        if (name == "__metadata__") continue;
        TensorInfo ti;
        ti.file = path;
        ti.dtype = t["dtype"].as_text();
        for (const auto& d : t["shape"].items()) ti.shape.push_back(d.as_u64());
        ti.offset = 8 + hlen + t["data_offsets"][0].as_u64();
        ti.bytes = t["data_offsets"][1].as_u64() - t["data_offsets"][0].as_u64();
        tensors_[name] = ti;
      }
    }
    return Ok();
  }

  bool has(const std::string& n) const { return tensors_.count(n) != 0; }
  const TensorInfo* info(const std::string& n) const {
    auto it = tensors_.find(n);
    return it == tensors_.end() ? nullptr : &it->second;
  }

  Result<std::vector<float>> f32(const std::string& n) {
    return read<float>(n, [](const uint8_t* p, uint64_t count, int kind, float* out) {
      for (uint64_t i = 0; i < count; ++i) {
        if (kind == 0) std::memcpy(&out[i], p + i * 4, 4);
        else { uint16_t h; std::memcpy(&h, p + i * 2, 2); out[i] = kind == 1 ? bf16_to_f32(h) : f16_to_f32(h); }
      }
    });
  }
  Result<std::vector<uint16_t>> bf16(const std::string& n) {
    return read<uint16_t>(n, [](const uint8_t* p, uint64_t count, int kind, uint16_t* out) {
      if (kind == 1) { std::memcpy(out, p, count * 2); return; }
      for (uint64_t i = 0; i < count; ++i) {
        if (kind == 2) { uint16_t h; std::memcpy(&h, p + i * 2, 2); out[i] = f32_to_bf16(f16_to_f32(h)); }
        else { float f; std::memcpy(&f, p + i * 4, 4); out[i] = f32_to_bf16(f); }
      }
    });
  }

 private:
  // conv(bytes, count, kind, out): kind 0 = F32, 1 = BF16, 2 = F16.
  template <class T, class Conv>
  Result<std::vector<T>> read(const std::string& name, Conv conv) {
    const TensorInfo* ti = info(name);
    if (!ti) return Result<std::vector<T>>(Status::NotFound, "tensor " + name + " is not in the checkpoint");
    if (ti->dtype != "BF16" && ti->dtype != "F16" && ti->dtype != "F32")
      return Result<std::vector<T>>(Status::Unsupported, name + ": dtype " + ti->dtype + " is not supported");
    auto raw = host_->read_range(ti->file, ti->offset, ti->bytes);
    if (!raw.ok()) return raw.error();
    if (raw->size() != ti->bytes) return Result<std::vector<T>>(Status::Invalid, name + ": the file ends inside the tensor");
    std::vector<T> out((size_t)ti->elements());
    conv(raw->data(), out.size(), ti->dtype == "F32" ? 0 : ti->dtype == "BF16" ? 1 : 2, out.data());
    return out;
  }
  hal::Host* host_ = nullptr;
  std::map<std::string, TensorInfo> tensors_;
};

// ---- arithmetic ----------------------------------------------------------

// A linear map y = W x (+ b) with W kept in bf16, plus an optional low-rank
// adapter y += scale * B (A x) kept in f32.
struct Linear {
  std::vector<uint16_t> w;       // [out, in]
  std::vector<float> bias;       // [out] or empty
  uint32_t out = 0, in = 0;
  std::vector<float> la, lb;     // adapter A [r, in], B [out, r]
  uint32_t rank = 0;
  float scale = 0;
};

#if defined(__GNUC__)
// Eight lanes as one value: GCC and Clang lower this to one AVX register, two
// SSE or NEON registers, or scalars, whatever the target has.
typedef float v8f __attribute__((vector_size(32)));
typedef uint16_t v8h __attribute__((vector_size(16)));
typedef uint32_t v8u __attribute__((vector_size(32)));

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpsabi"   // the vector helpers are always inlined; no call passes a v8f
namespace k_base {
#include "llm_kernels.inc"
}
#if AIOS_LLM_AVX2
#pragma GCC push_options
#pragma GCC target("avx2,fma")
namespace k_avx2 {
#include "llm_kernels.inc"
}
#pragma GCC pop_options
#endif
#pragma GCC diagnostic pop
#else
// MSVC has none of the GCC/Clang vector extensions llm_kernels.inc uses
// (__attribute__((vector_size)), __builtin_convertvector, __builtin_memcpy):
// a plain scalar fallback, same contract as k_base above.
namespace k_base {
static float dot_bf16(const uint16_t* w, const float* x, uint32_t n) {
  float s = 0.f;
  for (uint32_t i = 0; i < n; ++i) s += bf16_to_f32(w[i]) * x[i];
  return s;
}
static float dot_f32(const float* w, const float* x, uint32_t n) {
  float s = 0.f;
  for (uint32_t i = 0; i < n; ++i) s += w[i] * x[i];
  return s;
}
static void dot3x4_f32(const float* r0, const float* r1, const float* r2, const float* x0, const float* x1,
                        const float* x2, const float* x3, uint32_t n, float* out) {
  const float* rs[3] = {r0, r1, r2};
  const float* xs[4] = {x0, x1, x2, x3};
  float s[12] = {};
  for (uint32_t i = 0; i < n; ++i)
    for (int r = 0; r < 3; ++r)
      for (int t = 0; t < 4; ++t) s[r * 4 + t] += rs[r][i] * xs[t][i];
  for (int k = 0; k < 12; ++k) out[k] = s[k];
}
static void axpy_f32(float w, const float* v, float* out, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) out[i] += w * v[i];
}
static void widen_bf16(const uint16_t* w, float* out, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) out[i] = bf16_to_f32(w[i]);
}
}  // namespace k_base
#endif

// The kernels this machine runs, picked once: the AVX2+FMA build where the
// processor has both, the portable one everywhere else.
struct Kernels {
  float (*dot_bf16)(const uint16_t*, const float*, uint32_t);
  float (*dot_f32)(const float*, const float*, uint32_t);
  void (*dot3x4_f32)(const float*, const float*, const float*, const float*, const float*, const float*, const float*, uint32_t, float*);
  void (*axpy_f32)(float, const float*, float*, uint32_t);
  void (*widen_bf16)(const uint16_t*, float*, uint32_t);
  const char* name;
};
const Kernels& kernels() {
  static const Kernels k = [] {
#if AIOS_LLM_AVX2
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
      return Kernels{k_avx2::dot_bf16, k_avx2::dot_f32, k_avx2::dot3x4_f32, k_avx2::axpy_f32, k_avx2::widen_bf16, "avx2+fma"};
#endif
    return Kernels{k_base::dot_bf16, k_base::dot_f32, k_base::dot3x4_f32, k_base::axpy_f32, k_base::widen_bf16, "portable"};
  }();
  return k;
}
float dot_bf16(const uint16_t* w, const float* x, uint32_t n) { return kernels().dot_bf16(w, x, n); }
float dot_f32(const float* w, const float* x, uint32_t n) { return kernels().dot_f32(w, x, n); }

// Rows [lo, hi) of Y[T, rows] = X[T, in] * W^T, split over the lanes.
template <class RowFn>
void parallel_rows(uint32_t rows, const RowFn& fn) {
  const uint32_t lanes = std::max<uint32_t>(1, hal::task_lanes());
  const uint32_t chunks = std::min<uint32_t>(rows, lanes * 4);
  hal::run_tasks(chunks, [&](uint32_t c) {
    const uint32_t lo = (uint64_t)rows * c / chunks, hi = (uint64_t)rows * (c + 1) / chunks;
    fn(lo, hi);
  });
}

void apply_linear(const Linear& l, const float* X, uint32_t T, float* Y, std::vector<float>& scratch) {
  if (T == 1) {
    parallel_rows(l.out, [&](uint32_t lo, uint32_t hi) {
      for (uint32_t o = lo; o < hi; ++o) Y[o] = dot_bf16(&l.w[(size_t)o * l.in], X, l.in) + (l.bias.empty() ? 0.f : l.bias[o]);
    });
  } else {
    // A prompt: six rows are widened to f32 once and run against the
    // tokens 32 at a time, so neither the rows nor the activations stream out
    // of cache once per pairing.
    constexpr uint32_t kRows = 6, kTok = 32;
    parallel_rows((l.out + kRows - 1) / kRows, [&](uint32_t lo, uint32_t hi) {
      std::vector<float> wide((size_t)kRows * l.in);
      for (uint32_t blk = lo; blk < hi; ++blk) {
        const uint32_t o0 = blk * kRows, rows = std::min(kRows, l.out - o0);
        for (uint32_t r = 0; r < rows; ++r) kernels().widen_bf16(&l.w[(size_t)(o0 + r) * l.in], &wide[(size_t)r * l.in], l.in);
        for (uint32_t t0 = 0; t0 < T; t0 += kTok) {
          const uint32_t t1 = std::min(T, t0 + kTok);
          uint32_t r = 0;
          for (; r + 3 <= rows; r += 3) {
            const float* w0 = &wide[(size_t)r * l.in];
            const float* w1 = w0 + l.in;
            const float* w2 = w1 + l.in;
            const uint32_t o = o0 + r;
            float b[3];
            for (int k = 0; k < 3; ++k) b[k] = l.bias.empty() ? 0.f : l.bias[o + k];
            uint32_t t = t0;
            for (; t + 4 <= t1; t += 4) {
              const float* x = X + (size_t)t * l.in;
              float y[12];
              kernels().dot3x4_f32(w0, w1, w2, x, x + l.in, x + 2 * (size_t)l.in, x + 3 * (size_t)l.in, l.in, y);
              for (int k = 0; k < 3; ++k)
                for (int q = 0; q < 4; ++q) Y[(size_t)(t + q) * l.out + o + k] = y[k * 4 + q] + b[k];
            }
            for (; t < t1; ++t)
              for (int k = 0; k < 3; ++k) Y[(size_t)t * l.out + o + k] = dot_f32(w0 + (size_t)k * l.in, X + (size_t)t * l.in, l.in) + b[k];
          }
          for (; r < rows; ++r) {
            const float* w = &wide[(size_t)r * l.in];
            const uint32_t o = o0 + r;
            const float b = l.bias.empty() ? 0.f : l.bias[o];
            for (uint32_t t = t0; t < t1; ++t) Y[(size_t)t * l.out + o] = dot_f32(w, X + (size_t)t * l.in, l.in) + b;
          }
        }
      }
    });
  }
  if (l.rank) {
    scratch.assign((size_t)T * l.rank, 0.f);
    float* mid = scratch.data();
    parallel_rows(l.rank, [&](uint32_t lo, uint32_t hi) {
      for (uint32_t r = lo; r < hi; ++r)
        for (uint32_t t = 0; t < T; ++t) mid[(size_t)t * l.rank + r] = dot_f32(&l.la[(size_t)r * l.in], X + (size_t)t * l.in, l.in);
    });
    parallel_rows(l.out, [&](uint32_t lo, uint32_t hi) {
      for (uint32_t o = lo; o < hi; ++o)
        for (uint32_t t = 0; t < T; ++t)
          Y[(size_t)t * l.out + o] += l.scale * dot_f32(&l.lb[(size_t)o * l.rank], mid + (size_t)t * l.rank, l.rank);
    });
  }
}

void rms_norm(const float* x, const float* w, float eps, uint32_t n, float* y) {
  double ss = 0;
  for (uint32_t i = 0; i < n; ++i) ss += (double)x[i] * x[i];
  const float inv = 1.0f / std::sqrt((float)(ss / n) + eps);
  for (uint32_t i = 0; i < n; ++i) y[i] = x[i] * inv * w[i];
}

inline float silu(float x) { return x / (1.0f + std::exp(-x)); }

struct Layer {
  Linear q, k, v, o, gate, up, down;
  std::vector<float> ln1, ln2;
};

uint64_t splitmix(uint64_t& s) {
  uint64_t z = (s += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

size_t utf8_complete_prefix(const std::string& s) {
  // Length of the longest prefix that does not end inside a multi-byte sequence.
  size_t n = s.size();
  for (size_t back = 1; back <= 3 && back <= n; ++back) {
    const uint8_t c = (uint8_t)s[n - back];
    if ((c & 0xC0) == 0x80) continue;
    const size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    return need > back ? n - back : n;
  }
  return n;
}

}  // namespace

struct LlmModel::Impl {
  LlmConfig cfg;
  BpeTokenizer tok;
  std::vector<uint16_t> embed;          // [vocab, hidden], also the output head when tied
  std::vector<uint16_t> head;           // separate output head when not tied
  std::vector<float> final_norm;
  std::vector<Layer> layers;
  bool adapter = false;
  std::string system_prompt = "You are a helpful assistant.";
  SampleParams gen_defaults;
  uint32_t ctx = 0;
  uint64_t bytes = 0;
  hal::Mutex run;                       // one forward pass at a time: there is one cache

  std::vector<float> cos_t, sin_t;      // [ctx, head_dim/2]
  std::vector<std::vector<float>> kc, vc;   // per layer [ctx, kv_heads*head_dim]
  std::vector<int32_t> cached;          // tokens whose K/V are in the cache

  // One forward pass over `tokens` placed at positions pos0.... Fills the cache.
  // want_logits: the logits of the last token are written to `logits`.
  void forward(const int32_t* tokens, uint32_t T, uint32_t pos0, bool want_logits, std::vector<float>* logits) {
    const uint32_t H = cfg.hidden, hd = cfg.head_dim, nh = cfg.heads, nkv = cfg.kv_heads;
    const uint32_t qd = nh * hd, kvd = nkv * hd, half = hd / 2, group = nh / nkv;
    std::vector<float> x((size_t)T * H), xn((size_t)T * H), q((size_t)T * qd), k((size_t)T * kvd), v((size_t)T * kvd);
    std::vector<float> att((size_t)T * qd), o((size_t)T * H), g((size_t)T * cfg.ffn), u((size_t)T * cfg.ffn), scratch;
    for (uint32_t t = 0; t < T; ++t) {
      const uint16_t* e = &embed[(size_t)tokens[t] * H];
      for (uint32_t i = 0; i < H; ++i) x[(size_t)t * H + i] = bf16_to_f32(e[i]);
    }
    const float inv_sqrt = 1.0f / std::sqrt((float)hd);
    for (uint32_t li = 0; li < cfg.layers; ++li) {
      const Layer& L = layers[li];
      for (uint32_t t = 0; t < T; ++t) rms_norm(&x[(size_t)t * H], L.ln1.data(), cfg.rms_eps, H, &xn[(size_t)t * H]);
      apply_linear(L.q, xn.data(), T, q.data(), scratch);
      apply_linear(L.k, xn.data(), T, k.data(), scratch);
      apply_linear(L.v, xn.data(), T, v.data(), scratch);
      for (uint32_t t = 0; t < T; ++t) {
        const float* c = &cos_t[(size_t)(pos0 + t) * half];
        const float* s = &sin_t[(size_t)(pos0 + t) * half];
        for (uint32_t h = 0; h < nh; ++h) {
          float* p = &q[(size_t)t * qd + (size_t)h * hd];
          for (uint32_t i = 0; i < half; ++i) { const float a = p[i], b = p[i + half]; p[i] = a * c[i] - b * s[i]; p[i + half] = b * c[i] + a * s[i]; }
        }
        for (uint32_t h = 0; h < nkv; ++h) {
          float* p = &k[(size_t)t * kvd + (size_t)h * hd];
          for (uint32_t i = 0; i < half; ++i) { const float a = p[i], b = p[i + half]; p[i] = a * c[i] - b * s[i]; p[i + half] = b * c[i] + a * s[i]; }
        }
        std::memcpy(&kc[li][(size_t)(pos0 + t) * kvd], &k[(size_t)t * kvd], kvd * sizeof(float));
        std::memcpy(&vc[li][(size_t)(pos0 + t) * kvd], &v[(size_t)t * kvd], kvd * sizeof(float));
      }
      // Attention: every (token, head) pair is independent.
      const uint32_t jobs = T * nh;
      hal::run_tasks(std::min<uint32_t>(jobs, std::max<uint32_t>(1, hal::task_lanes()) * 4), [&](uint32_t chunk) {
        const uint32_t nchunk = std::min<uint32_t>(jobs, std::max<uint32_t>(1, hal::task_lanes()) * 4);
        std::vector<float> sc(pos0 + T);
        for (uint32_t j = (uint64_t)jobs * chunk / nchunk; j < (uint64_t)jobs * (chunk + 1) / nchunk; ++j) {
          const uint32_t t = j / nh, h = j % nh, kvh = h / group, len = pos0 + t + 1;
          const float* qh = &q[(size_t)t * qd + (size_t)h * hd];
          float mx = -1e30f;
          for (uint32_t p = 0; p < len; ++p) {
            const float s = dot_f32(&kc[li][(size_t)p * kvd + (size_t)kvh * hd], qh, hd) * inv_sqrt;
            sc[p] = s;
            mx = std::max(mx, s);
          }
          float sum = 0;
          for (uint32_t p = 0; p < len; ++p) { sc[p] = std::exp(sc[p] - mx); sum += sc[p]; }
          float* out = &att[(size_t)t * qd + (size_t)h * hd];
          for (uint32_t i = 0; i < hd; ++i) out[i] = 0;
          for (uint32_t p = 0; p < len; ++p) {
            const float w = sc[p] / sum;
            kernels().axpy_f32(w, &vc[li][(size_t)p * kvd + (size_t)kvh * hd], out, hd);
          }
        }
      });
      apply_linear(L.o, att.data(), T, o.data(), scratch);
      for (size_t i = 0; i < (size_t)T * H; ++i) x[i] += o[i];
      for (uint32_t t = 0; t < T; ++t) rms_norm(&x[(size_t)t * H], L.ln2.data(), cfg.rms_eps, H, &xn[(size_t)t * H]);
      apply_linear(L.gate, xn.data(), T, g.data(), scratch);
      apply_linear(L.up, xn.data(), T, u.data(), scratch);
      for (size_t i = 0; i < (size_t)T * cfg.ffn; ++i) g[i] = silu(g[i]) * u[i];
      apply_linear(L.down, g.data(), T, o.data(), scratch);
      for (size_t i = 0; i < (size_t)T * H; ++i) x[i] += o[i];
    }
    if (!want_logits) return;
    std::vector<float> last(H);
    rms_norm(&x[(size_t)(T - 1) * H], final_norm.data(), cfg.rms_eps, H, last.data());
    const std::vector<uint16_t>& W = cfg.tied_head ? embed : head;
    logits->assign(cfg.vocab, 0.f);
    parallel_rows(cfg.vocab, [&](uint32_t lo, uint32_t hi) {
      for (uint32_t r = lo; r < hi; ++r) (*logits)[r] = dot_bf16(&W[(size_t)r * H], last.data(), H);
    });
  }

  uint32_t sample(std::vector<float>& lg, const SampleParams& p, uint64_t& rng) {
    if (p.repetition_penalty > 0.f && p.repetition_penalty != 1.f) {
      std::vector<uint8_t> seen(lg.size(), 0);
      for (int32_t t : cached) {
        if (t < 0 || (size_t)t >= lg.size() || seen[(size_t)t]) continue;
        seen[(size_t)t] = 1;
        float& v = lg[(size_t)t];
        v = v < 0 ? v * p.repetition_penalty : v / p.repetition_penalty;
      }
    }
    if (p.temperature <= 0.f) return (uint32_t)(std::max_element(lg.begin(), lg.end()) - lg.begin());
    std::vector<uint32_t> idx(lg.size());
    std::iota(idx.begin(), idx.end(), 0u);
    const size_t keep = p.top_k && p.top_k < idx.size() ? p.top_k : std::min<size_t>(idx.size(), 4096);
    std::partial_sort(idx.begin(), idx.begin() + (long)keep, idx.end(), [&](uint32_t a, uint32_t b) { return lg[a] > lg[b]; });
    idx.resize(keep);
    const float mx = lg[idx[0]];
    std::vector<double> pr(keep);
    double sum = 0;
    for (size_t i = 0; i < keep; ++i) { pr[i] = std::exp((double)(lg[idx[i]] - mx) / p.temperature); sum += pr[i]; }
    size_t cut = keep;
    if (p.top_p < 1.0f) {
      double acc = 0;
      for (size_t i = 0; i < keep; ++i) { acc += pr[i] / sum; if (acc >= p.top_p) { cut = i + 1; break; } }
    }
    double tot = 0;
    for (size_t i = 0; i < cut; ++i) tot += pr[i];
    const double r = (double)(splitmix(rng) >> 11) / 9007199254740992.0 * tot;
    double acc = 0;
    for (size_t i = 0; i < cut; ++i) { acc += pr[i]; if (r < acc) return idx[i]; }
    return idx[cut - 1];
  }
};

LlmModel::LlmModel() : impl_(new Impl) {}
LlmModel::~LlmModel() = default;
const LlmConfig& LlmModel::config() const { return impl_->cfg; }
const BpeTokenizer& LlmModel::tokenizer() const { return impl_->tok; }
uint32_t LlmModel::context() const { return impl_->ctx; }
bool LlmModel::has_adapter() const { return impl_->adapter; }
uint64_t LlmModel::weight_bytes() const { return impl_->bytes; }

const std::string& LlmModel::default_system() const { return impl_->system_prompt; }
const SampleParams& LlmModel::defaults() const { return impl_->gen_defaults; }

std::string LlmModel::chat_prompt(const std::vector<ChatMessage>& messages) const {
  std::string s;
  if (messages.empty() || messages.front().role != "system")
    s += "<|im_start|>system\n" + impl_->system_prompt + "<|im_end|>\n";
  for (const auto& m : messages) s += "<|im_start|>" + m.role + "\n" + m.content + "<|im_end|>\n";
  s += "<|im_start|>assistant\n";
  return s;
}

namespace {
Result<RpcValue> read_json(hal::Host& host, const std::string& path) {
  auto raw = host.read_file(path);
  if (!raw.ok()) {
    const std::string& why = raw.error().message;
    return Result<RpcValue>(raw.error().status, why.find(path) != std::string::npos ? why : path + ": " + why);
  }
  auto doc = channel::rpc_parse(ByteSpan(raw->data(), raw->size()));
  if (!doc.ok()) return Result<RpcValue>(Status::Invalid, path + ": " + doc.error().message);
  return doc;
}
// The default system turn written into a chat template: the literal
// '<|im_start|>system\n...<|im_end|>' the template emits when the conversation
// brings no system message of its own. Empty when the template has none.
std::string template_default_system(const std::string& tpl) {
  const std::string open = "'<|im_start|>system\\n";
  for (size_t at = tpl.find(open); at != std::string::npos; at = tpl.find(open, at + 1)) {
    const size_t from = at + open.size();
    if (from < tpl.size() && tpl[from] == '\'') continue;   // the branch that splices in messages[0]
    const size_t end = tpl.find("<|im_end|>", from);
    const size_t quote = tpl.find('\'', from);
    if (end == std::string::npos || quote < end) continue;
    std::string raw = tpl.substr(from, end - from), out;
    for (size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == 'n') { out.push_back('\n'); ++i; }
      else out.push_back(raw[i]);
    }
    return out;
  }
  return {};
}
}  // namespace

Result<std::unique_ptr<LlmModel>> LlmModel::load(hal::Host& host, const std::string& dir,
                                                 const std::string& adapter_dir, uint32_t context) {
  using R = Result<std::unique_ptr<LlmModel>>;
  if (!host.exists(dir + "/config.json"))
    return R(Status::NotFound, "no model at " + dir + ": put a Qwen2 checkpoint there (config.json, *.safetensors, tokenizer.json) or set AIOS_LLM_MODEL");
  auto cj = read_json(host, dir + "/config.json");
  if (!cj.ok()) return R(cj.error().status, cj.error().message);
  std::unique_ptr<LlmModel> m(new LlmModel);
  Impl& d = *m->impl_;
  LlmConfig& c = d.cfg;
  const std::string arch = (*cj)["model_type"].as_text();
  if (arch != "qwen2") return R(Status::Unsupported, "model_type \"" + arch + "\" is not supported (qwen2 only)");
  c.vocab = (uint32_t)(*cj)["vocab_size"].as_u64();
  c.hidden = (uint32_t)(*cj)["hidden_size"].as_u64();
  c.layers = (uint32_t)(*cj)["num_hidden_layers"].as_u64();
  c.heads = (uint32_t)(*cj)["num_attention_heads"].as_u64();
  c.kv_heads = (uint32_t)(*cj)["num_key_value_heads"].as_u64();
  c.ffn = (uint32_t)(*cj)["intermediate_size"].as_u64();
  c.max_positions = (uint32_t)(*cj)["max_position_embeddings"].as_u64();
  c.rms_eps = (float)(*cj)["rms_norm_eps"].as_double(1e-6);
  c.rope_theta = (float)(*cj)["rope_theta"].as_double(1e6);
  c.tied_head = (*cj)["tie_word_embeddings"].truthy();
  if (!c.vocab || !c.hidden || !c.layers || !c.heads || !c.kv_heads || !c.ffn || c.hidden % c.heads || c.heads % c.kv_heads)
    return R(Status::Invalid, "config.json does not describe a valid qwen2 model");
  c.head_dim = c.hidden / c.heads;
  if (c.head_dim % 2) return R(Status::Invalid, "head_dim must be even for rotary embeddings");
  d.ctx = std::min<uint32_t>(context ? context : 4096, c.max_positions ? c.max_positions : 4096);

  // A fine-tune ships the tokenizer files it was trained with; they win over the base's.
  const std::string tok_dir = !adapter_dir.empty() && host.exists(adapter_dir + "/tokenizer.json") ? adapter_dir : dir;
  const std::string cfg_dir = !adapter_dir.empty() && host.exists(adapter_dir + "/tokenizer_config.json") ? adapter_dir : dir;
  auto tj = host.read_file(tok_dir + "/tokenizer.json");
  if (!tj.ok()) return R(tj.error().status, tok_dir + "/tokenizer.json: " + tj.error().message);
  if (host.exists(cfg_dir + "/tokenizer_config.json")) {
    auto tc = read_json(host, cfg_dir + "/tokenizer_config.json");
    if (tc.ok()) {
      const std::string sys = template_default_system((*tc)["chat_template"].as_text());
      if (!sys.empty()) d.system_prompt = sys;
    }
  }
  if (host.exists(dir + "/generation_config.json")) {
    auto gc = read_json(host, dir + "/generation_config.json");
    if (gc.ok()) {
      SampleParams& g = d.gen_defaults;
      if ((*gc).has("temperature")) g.temperature = (float)(*gc)["temperature"].as_double(g.temperature);
      if ((*gc).has("top_p")) g.top_p = (float)(*gc)["top_p"].as_double(g.top_p);
      if ((*gc).has("top_k")) g.top_k = (uint32_t)(*gc)["top_k"].as_u64(g.top_k);
      if ((*gc).has("repetition_penalty")) g.repetition_penalty = (float)(*gc)["repetition_penalty"].as_double(1.0);
      if ((*gc).has("do_sample") && !(*gc)["do_sample"].truthy()) g.temperature = 0.f;
    }
  }
  auto tk = BpeTokenizer::from_json(std::string(tj->begin(), tj->end()));
  if (!tk.ok()) return R(tk.error().status, tk.error().message);
  d.tok = std::move(*tk);

  Safetensors st;
  if (auto r = st.open_dir(host, dir); !r.ok()) return R(r.error().status, r.error().message);

#define TRY(var, expr) auto var = (expr); if (!var.ok()) return R(var.error().status, var.error().message)
  TRY(emb, st.bf16("model.embed_tokens.weight"));
  d.embed = std::move(*emb);
  if (d.embed.size() != (size_t)c.vocab * c.hidden) return R(Status::Invalid, "embedding table does not match config.json");
  if (!c.tied_head) { TRY(hd, st.bf16("lm_head.weight")); d.head = std::move(*hd); }
  TRY(fn, st.f32("model.norm.weight"));
  d.final_norm = std::move(*fn);

  auto lin = [&](Linear& l, const std::string& name, uint32_t out, uint32_t in, bool bias) -> Result<void> {
    auto w = st.bf16(name + ".weight");
    if (!w.ok()) return w.error();
    if (w->size() != (size_t)out * in) return Result<void>(Status::Invalid, name + ": shape does not match config.json");
    l.w = std::move(*w);
    l.out = out;
    l.in = in;
    if (bias) {
      auto b = st.f32(name + ".bias");
      if (!b.ok()) return b.error();
      l.bias = std::move(*b);
    }
    return Ok();
  };
  const uint32_t H = c.hidden, kvd = c.kv_heads * c.head_dim;
  d.layers.resize(c.layers);
  for (uint32_t i = 0; i < c.layers; ++i) {
    const std::string p = "model.layers." + std::to_string(i) + ".";
    Layer& L = d.layers[i];
    for (auto r : {lin(L.q, p + "self_attn.q_proj", H, H, true), lin(L.k, p + "self_attn.k_proj", kvd, H, true),
                   lin(L.v, p + "self_attn.v_proj", kvd, H, true), lin(L.o, p + "self_attn.o_proj", H, H, false),
                   lin(L.gate, p + "mlp.gate_proj", c.ffn, H, false), lin(L.up, p + "mlp.up_proj", c.ffn, H, false),
                   lin(L.down, p + "mlp.down_proj", H, c.ffn, false)})
      if (!r.ok()) return R(r.error().status, r.error().message);
    TRY(a, st.f32(p + "input_layernorm.weight"));
    TRY(b, st.f32(p + "post_attention_layernorm.weight"));
    L.ln1 = std::move(*a);
    L.ln2 = std::move(*b);
  }

  if (!adapter_dir.empty()) {
    auto aj = read_json(host, adapter_dir + "/adapter_config.json");
    if (!aj.ok()) return R(aj.error().status, aj.error().message);
    if ((*aj)["use_dora"].truthy()) return R(Status::Unsupported, "DoRA adapters are not supported");
    const double rank = (*aj)["r"].as_double(0), alpha = (*aj)["lora_alpha"].as_double(rank);
    if (rank <= 0) return R(Status::Invalid, "adapter_config.json has no rank");
    const float scale = (*aj)["use_rslora"].truthy() ? (float)(alpha / std::sqrt(rank)) : (float)(alpha / rank);
    Safetensors ad;
    if (auto r = ad.open_dir(host, adapter_dir); !r.ok()) return R(r.error().status, r.error().message);
    size_t applied = 0;
    for (uint32_t i = 0; i < c.layers; ++i) {
      const std::string p = "base_model.model.model.layers." + std::to_string(i) + ".";
      struct Target { const char* path; Linear* l; };
      Layer& L = d.layers[i];
      for (const Target& t : {Target{"self_attn.q_proj", &L.q}, Target{"self_attn.k_proj", &L.k}, Target{"self_attn.v_proj", &L.v},
                              Target{"self_attn.o_proj", &L.o}, Target{"mlp.gate_proj", &L.gate}, Target{"mlp.up_proj", &L.up},
                              Target{"mlp.down_proj", &L.down}}) {
        const std::string base = p + t.path;
        if (!ad.has(base + ".lora_A.weight")) continue;
        TRY(A, ad.f32(base + ".lora_A.weight"));
        TRY(B, ad.f32(base + ".lora_B.weight"));
        const TensorInfo* ai = ad.info(base + ".lora_A.weight");
        const TensorInfo* bi = ad.info(base + ".lora_B.weight");
        if (ai->shape.size() != 2 || bi->shape.size() != 2 || ai->shape[1] != t.l->in || bi->shape[0] != t.l->out || ai->shape[0] != bi->shape[1])
          return R(Status::Invalid, base + ": adapter shapes do not fit the base weight");
        t.l->la = std::move(*A);
        t.l->lb = std::move(*B);
        t.l->rank = (uint32_t)ai->shape[0];
        t.l->scale = scale;
        ++applied;
      }
    }
    if (!applied) return R(Status::Invalid, "the adapter holds no tensor for this model");
    d.adapter = true;
  }
#undef TRY

  const uint32_t half = c.head_dim / 2;
  d.cos_t.resize((size_t)d.ctx * half);
  d.sin_t.resize((size_t)d.ctx * half);
  for (uint32_t pos = 0; pos < d.ctx; ++pos)
    for (uint32_t i = 0; i < half; ++i) {
      const double f = std::pow((double)c.rope_theta, -2.0 * i / c.head_dim) * pos;
      d.cos_t[(size_t)pos * half + i] = (float)std::cos(f);
      d.sin_t[(size_t)pos * half + i] = (float)std::sin(f);
    }
  d.kc.assign(c.layers, std::vector<float>((size_t)d.ctx * kvd));
  d.vc.assign(c.layers, std::vector<float>((size_t)d.ctx * kvd));

  d.bytes = d.embed.size() * 2 + d.head.size() * 2;
  for (const auto& L : d.layers)
    for (const Linear* l : {&L.q, &L.k, &L.v, &L.o, &L.gate, &L.up, &L.down})
      d.bytes += l->w.size() * 2 + (l->la.size() + l->lb.size()) * 4;
  return m;
}

Result<std::vector<float>> LlmModel::logits(const std::vector<int32_t>& tokens) {
  Impl& d = *impl_;
  hal::LockGuard hold(d.run);
  if (tokens.empty() || tokens.size() > d.ctx) return Result<std::vector<float>>(Status::Invalid, "token count is outside the context window");
  for (int32_t t : tokens) if (t < 0 || (uint32_t)t >= d.cfg.vocab) return Result<std::vector<float>>(Status::Invalid, "token id outside the vocabulary");
  d.cached.clear();
  std::vector<float> lg;
  for (size_t at = 0; at < tokens.size(); at += 64) {
    const uint32_t T = (uint32_t)std::min<size_t>(64, tokens.size() - at);
    d.forward(&tokens[at], T, (uint32_t)at, at + T == tokens.size(), &lg);
  }
  d.cached = tokens;
  return lg;
}

Result<GenerateResult> LlmModel::generate(hal::Host& host, const std::string& prompt, const SampleParams& p,
                                          const std::function<bool(const std::string&)>& on_text) {
  using R = Result<GenerateResult>;
  Impl& d = *impl_;
  hal::LockGuard hold(d.run);
  std::vector<int32_t> ids = d.tok.encode(prompt);
  if (ids.empty()) return R(Status::Invalid, "the prompt is empty");
  for (int32_t t : ids) if ((uint32_t)t >= d.cfg.vocab) return R(Status::Invalid, "the prompt holds a token the model has no embedding for");
  if (ids.size() >= d.ctx) return R(Status::Invalid, "the prompt is " + std::to_string(ids.size()) + " tokens; the context window is " + std::to_string(d.ctx));
  const uint32_t room = d.ctx - (uint32_t)ids.size();
  const uint32_t limit = std::min<uint32_t>(p.max_tokens ? p.max_tokens : room, room);

  GenerateResult res;
  res.prompt_tokens = (uint32_t)ids.size();
  const uint64_t t0 = host.monotonic_ns();

  // Keep whatever of the cache the new prompt shares, but always run the last
  // prompt token so there are logits to sample from.
  size_t keep = 0;
  while (keep < d.cached.size() && keep + 1 < ids.size() && d.cached[keep] == ids[keep]) ++keep;
  d.cached.resize(keep);
  std::vector<float> lg;
  for (size_t at = keep; at < ids.size(); at += 64) {
    const uint32_t T = (uint32_t)std::min<size_t>(64, ids.size() - at);
    d.forward(&ids[at], T, (uint32_t)at, at + T == ids.size(), &lg);
  }
  d.cached = ids;
  const uint64_t t1 = host.monotonic_ns();
  res.prefill_ms = (double)(t1 - t0) / 1e6;

  uint64_t rng = p.seed;
  if (!rng) { uint8_t b[8]; host.random(MutableByteSpan(b, 8)); for (int i = 0; i < 8; ++i) rng = (rng << 8) | b[i]; }
  const int32_t eos_a = d.tok.token_id("<|im_end|>"), eos_b = d.tok.token_id("<|endoftext|>");

  std::string pending;   // decoded bytes not yet handed to the caller
  size_t emit_hold = 0;  // longest stop string: that many trailing bytes wait for a possible match
  for (const auto& s : p.stop) emit_hold = std::max(emit_hold, s.size());
  bool stopped = false;
  auto flush = [&](bool final_flush) {
    size_t upto = pending.size() > emit_hold ? pending.size() - emit_hold : 0;
    if (final_flush) upto = pending.size();
    upto = std::min(upto, utf8_complete_prefix(pending.substr(0, upto)));
    if (upto == 0) return;
    const std::string piece = pending.substr(0, upto);
    pending.erase(0, upto);
    res.text += piece;
    if (on_text && !on_text(piece)) { stopped = true; res.cancelled = true; }
  };

  uint32_t produced = 0;
  while (produced < limit && !stopped) {
    const uint32_t next = d.sample(lg, p, rng);
    if ((int32_t)next == eos_a || (int32_t)next == eos_b) break;
    ++produced;
    pending += d.tok.token_bytes((int32_t)next);
    bool hit_stop = false;
    for (const auto& s : p.stop) {
      if (s.empty()) continue;
      const size_t at = pending.find(s);
      if (at != std::string::npos) { pending.resize(at); hit_stop = true; break; }
    }
    if (hit_stop) { flush(true); stopped = true; break; }
    flush(false);
    if (produced == limit) { res.hit_limit = true; break; }
    const int32_t tk = (int32_t)next;
    d.forward(&tk, 1, (uint32_t)d.cached.size(), true, &lg);
    d.cached.push_back(tk);
  }
  if (!stopped) flush(true);
  res.completion_tokens = produced;
  res.decode_ms = (double)(host.monotonic_ns() - t1) / 1e6;
  return res;
}

namespace {
struct Resident {
  hal::Mutex mu;
  std::unique_ptr<LlmModel> model;
  std::string source;
};
Resident& resident() { static Resident r; return r; }
}  // namespace

Result<LlmModel*> resident_llm_load(hal::Host& host, const std::string& dir, const std::string& adapter_dir, uint32_t context) {
  auto m = LlmModel::load(host, dir, adapter_dir, context);
  if (!m.ok()) return m.error();
  Resident& r = resident();
  hal::LockGuard g(r.mu);
  r.model = std::move(*m);
  r.source = adapter_dir.empty() ? dir : dir + " + adapter " + adapter_dir;
  return r.model.get();
}

Result<LlmModel*> resident_llm(hal::Host& host, const std::string& kernel_root) {
  {
    Resident& r = resident();
    hal::LockGuard g(r.mu);
    if (r.model) return r.model.get();
  }
  hal::Host& env = hal::process_host();
  std::string dir = env.env("AIOS_LLM_MODEL");
  if (dir.empty()) dir = kernel_root + "/models/default";
  return resident_llm_load(host, dir, env.env("AIOS_LLM_ADAPTER"));
}

LlmModel* resident_llm_loaded() {
  Resident& r = resident();
  hal::LockGuard g(r.mu);
  return r.model.get();
}

std::string resident_llm_source() {
  Resident& r = resident();
  hal::LockGuard g(r.mu);
  return r.source;
}

}  // namespace aios::userland
