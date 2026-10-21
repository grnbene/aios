// The CPU device: a lane pool over host threads executing the kernel's native
// operation set on tensors in fabric memory.
#include "aios/compute/device.hpp"
#include "aios/hal/sync.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/lane_pool.hpp"
#include <atomic>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <deque>

namespace aios {
namespace {

// Splits [0,n) across `lanes` host threads.
template <typename F>
void parallel_for(uint64_t n, uint32_t lanes, F f) {
  if (n == 0) return;
  lanes = std::max<uint32_t>(1, std::min<uint32_t>(lanes, (uint32_t)std::min<uint64_t>(n, 256)));
  if (lanes == 1) { f(0, n); return; }
  // the same slices as ever, run by the process's worker pool rather than a
  // thread created and joined per slice
  uint64_t chunk = (n + lanes - 1) / lanes;
  const uint32_t slices = (uint32_t)((n + chunk - 1) / chunk);
  hal::run_tasks(slices, [&](uint32_t l) {
    uint64_t a = l * chunk, b = std::min(n, a + chunk);
    if (a < b) f(a, b);
  });
}

template <typename T> T* at(MemoryFabric::Window& w, const Tensor& t) { return reinterpret_cast<T*>(w.base + t.offset); }

template <typename T>
Result<void> elementwise(OpCode code, const Operation& op, std::vector<MemoryFabric::Window>& in, std::vector<MemoryFabric::Window>& out, uint32_t lanes) {
  uint64_t n = op.outputs[0].elements();
  T* o = at<T>(out[0], op.outputs[0]);
  switch (code) {
    case OpCode::Fill: { T v = (T)op.scalar; parallel_for(n, lanes, [&](uint64_t a, uint64_t b) { for (uint64_t i = a; i < b; ++i) o[i] = v; }); return Ok(); }
    case OpCode::Copy: { const T* x = at<T>(in[0], op.inputs[0]); parallel_for(n, lanes, [&](uint64_t a, uint64_t b) { std::memcpy(o + a, x + a, (b - a) * sizeof(T)); }); return Ok(); }
    case OpCode::Scale: { const T* x = at<T>(in[0], op.inputs[0]); T s = (T)op.scalar; parallel_for(n, lanes, [&](uint64_t a, uint64_t b) { for (uint64_t i = a; i < b; ++i) o[i] = x[i] * s; }); return Ok(); }
    case OpCode::Relu: { const T* x = at<T>(in[0], op.inputs[0]); parallel_for(n, lanes, [&](uint64_t a, uint64_t b) { for (uint64_t i = a; i < b; ++i) o[i] = x[i] > T(0) ? x[i] : T(0); }); return Ok(); }
    case OpCode::Add: case OpCode::Mul: {
      const T* x = at<T>(in[0], op.inputs[0]); const T* y = at<T>(in[1], op.inputs[1]);
      bool mul = code == OpCode::Mul;
      parallel_for(n, lanes, [&](uint64_t a, uint64_t b) { for (uint64_t i = a; i < b; ++i) o[i] = mul ? x[i] * y[i] : x[i] + y[i]; });
      return Ok();
    }
    default: return Result<void>(Status::Unsupported);
  }
}

template <typename T>
void matmul(const T* A, const T* B, T* C, uint64_t M, uint64_t K, uint64_t N, uint32_t lanes) {
  parallel_for(M, lanes, [&](uint64_t r0, uint64_t r1) {
    for (uint64_t i = r0; i < r1; ++i) {
      T* c = C + i * N;
      for (uint64_t j = 0; j < N; ++j) c[j] = T(0);
      for (uint64_t k = 0; k < K; ++k) {
        T a = A[i * K + k];
        const T* b = B + k * N;
        for (uint64_t j = 0; j < N; ++j) c[j] += a * b[j];
      }
    }
  });
}

class CPUDevice final : public Device {
 public:
  CPUDevice(const DeviceID& id, uint32_t lanes, uint64_t memory, hal::Host* host = nullptr)
      : id_(id), name_("cpu"), host_(host) {
    cap_.lanes = lanes; cap_.memory = memory; cap_.native_memory = MemoryType::System; cap_.reads_host_memory = true;
    cap_.flops_per_second = uint64_t(lanes) * 4ull * 1000000000ull;  // conservative scalar estimate
  }
  const DeviceID& id() const override { return id_; }
  DeviceType type() const override { return DeviceType::CPU; }
  const std::string& name() const override { return name_; }
  const DeviceCapacity& capacity() const override { return cap_; }
  uint32_t busy_lanes() const override { return busy_.load(); }

  Result<DeviceMemoryStats> memory_stats() const override {
    DeviceMemoryStats s;
    s.total_bytes = cap_.memory;
    if (host_) {
      uint64_t avail = host_->available_bytes();
      s.free_bytes = avail;
      s.used_bytes = (s.total_bytes > avail) ? (s.total_bytes - avail) : 0;
      s.source = "host-available";
    } else {
      s.free_bytes = cap_.memory;
      s.used_bytes = 0;
    }
    return s;
  }

  bool supports(const Operation& op) const override {
    if (op.code == OpCode::Custom) { hal::LockGuard g(mu_); return kernels_.count(op.custom_kernel) > 0; }
    return true;
  }

  Result<void> register_kernel(const std::string& name, CustomKernel k) override {
    note_custom_kernel_name(name);
    hal::LockGuard g(mu_);
    kernels_[fnv1a(name)] = std::move(k);
    return Ok();
  }  std::vector<uint64_t> custom_kernels() const override {
    hal::LockGuard g(mu_);
    std::vector<uint64_t> v;
    for (const auto& k : kernels_) v.push_back(k.first);
    std::sort(v.begin(), v.end());
    return v;
  }


  Result<void> execute(const Operation& op, MemoryFabric& fabric, uint32_t lanes) override {
    lanes = std::max<uint32_t>(1, std::min(lanes, cap_.lanes));
    struct Busy { std::atomic<uint32_t>& b; uint32_t n; Busy(std::atomic<uint32_t>& x, uint32_t k) : b(x), n(k) { b += n; } ~Busy() { b -= n; } } busy(busy_, lanes);
    std::vector<MemoryFabric::Window> in, out;
    for (auto& t : op.inputs) { auto w = fabric.window(t.memory); if (!w.ok()) return w.error(); if (t.offset + t.bytes() > w->size) return Result<void>(Status::Invalid, "input tensor exceeds its memory object"); in.push_back(*w); }
    for (auto& t : op.outputs) { auto w = fabric.window(t.memory); if (!w.ok()) return w.error(); if (t.offset + t.bytes() > w->size) return Result<void>(Status::Invalid, "output tensor exceeds its memory object"); out.push_back(*w); }
    auto r = run(op, in, out, lanes);
    if (r.ok()) for (auto& t : op.outputs) fabric.touch(t.memory);
    return r;
  }

 private:
  Result<void> run(const Operation& op, std::vector<MemoryFabric::Window>& in, std::vector<MemoryFabric::Window>& out, uint32_t lanes) {
    auto need = [&](size_t ni, size_t no) -> Result<void> {
      if (op.inputs.size() < ni) return Result<void>(Status::Invalid, std::string(opcode_name(op.code)) + " needs " + std::to_string(ni) + " input(s)");
      if (op.outputs.size() < no) return Result<void>(Status::Invalid, std::string(opcode_name(op.code)) + " needs " + std::to_string(no) + " output(s)");
      return Ok();
    };
    switch (op.code) {
      case OpCode::Nop: return Ok();
      case OpCode::Fill: AIOS_TRY(need(0, 1)); return dispatch_elementwise(op, in, out, lanes);
      case OpCode::Copy: case OpCode::Scale: case OpCode::Relu: {
        AIOS_TRY(need(1, 1));
        if (op.inputs[0].elements() != op.outputs[0].elements()) return Result<void>(Status::Invalid, "shape mismatch");
        return dispatch_elementwise(op, in, out, lanes);
      }
      case OpCode::Gelu: {
        AIOS_TRY(need(1, 1));
        if (op.inputs[0].elements() != op.outputs[0].elements()) return Result<void>(Status::Invalid, "shape mismatch");
        const float* x = at<float>(in[0], op.inputs[0]);
        float* o = at<float>(out[0], op.outputs[0]);
        uint64_t n = op.outputs[0].elements();
        parallel_for(n, lanes, [&](uint64_t a, uint64_t b) {
          const float c = 0.7978845608f;
          for (uint64_t i = a; i < b; ++i) {
            float xi = x[i];
            float inner = c * (xi + 0.044715f * xi * xi * xi);
            o[i] = 0.5f * xi * (1.0f + std::tanh(inner));
          }
        });
        return Ok();
      }
      case OpCode::GeluGrad: {
        AIOS_TRY(need(2, 1));
        if (op.inputs[0].elements() != op.outputs[0].elements() || op.inputs[1].elements() != op.outputs[0].elements()) return Result<void>(Status::Invalid, "shape mismatch");
        const float* dy = at<float>(in[0], op.inputs[0]);
        const float* x = at<float>(in[1], op.inputs[1]);
        float* dx = at<float>(out[0], op.outputs[0]);
        uint64_t n = op.outputs[0].elements();
        parallel_for(n, lanes, [&](uint64_t a, uint64_t b) {
          const float c = 0.7978845608f;
          for (uint64_t i = a; i < b; ++i) {
            float xi = x[i];
            float x3 = xi * xi * xi;
            float inner = c * (xi + 0.044715f * x3);
            float t = std::tanh(inner);
            float dinner = c * (1.0f + 3.0f * 0.044715f * xi * xi);
            float dg = 0.5f * (1.0f + t) + 0.5f * xi * (1.0f - t * t) * dinner;
            dx[i] = dy[i] * dg;
          }
        });
        return Ok();
      }
      case OpCode::ReluGrad: {
        AIOS_TRY(need(2, 1));
        if (op.inputs[0].elements() != op.outputs[0].elements() || op.inputs[1].elements() != op.outputs[0].elements()) return Result<void>(Status::Invalid, "shape mismatch");
        const float* dy = at<float>(in[0], op.inputs[0]);
        const float* x = at<float>(in[1], op.inputs[1]);
        float* dx = at<float>(out[0], op.outputs[0]);
        uint64_t n = op.outputs[0].elements();
        parallel_for(n, lanes, [&](uint64_t a, uint64_t b) {
          for (uint64_t i = a; i < b; ++i) dx[i] = x[i] > 0.0f ? dy[i] : 0.0f;
        });
        return Ok();
      }
      case OpCode::Add: case OpCode::Mul: {
        AIOS_TRY(need(2, 1));
        if (op.inputs[0].elements() != op.outputs[0].elements() || op.inputs[1].elements() != op.outputs[0].elements()) return Result<void>(Status::Invalid, "shape mismatch");
        return dispatch_elementwise(op, in, out, lanes);
      }
      case OpCode::BiasAdd: {
        AIOS_TRY(need(2, 1));
        const Tensor& X = op.inputs[0]; const Tensor& Bias = op.inputs[1]; const Tensor& Y = op.outputs[0];
        if (Bias.elements() == 0 || X.elements() != Y.elements()) return Result<void>(Status::Invalid, "bias-add shape mismatch");
        uint64_t N = Bias.elements();
        uint64_t M = X.elements() / N;
        const float* x = at<float>(in[0], X);
        const float* b = at<float>(in[1], Bias);
        float* y = at<float>(out[0], Y);
        parallel_for(M, lanes, [&](uint64_t r0, uint64_t r1) {
          for (uint64_t r = r0; r < r1; ++r) {
            for (uint64_t c = 0; c < N; ++c) y[r * N + c] = x[r * N + c] + b[c];
          }
        });
        return Ok();
      }
      case OpCode::BiasAddGrad: {
        AIOS_TRY(need(1, 1));
        const Tensor& dY = op.inputs[0]; const Tensor& dBias = op.outputs[0];
        uint64_t N = dBias.elements();
        if (N == 0) return Result<void>(Status::Invalid, "bias-add-grad invalid output size");
        uint64_t M = dY.elements() / N;
        const float* dy = at<float>(in[0], dY);
        float* db = at<float>(out[0], dBias);
        parallel_for(N, lanes, [&](uint64_t c0, uint64_t c1) {
          for (uint64_t c = c0; c < c1; ++c) {
            float sum = 0.0f;
            for (uint64_t r = 0; r < M; ++r) sum += dy[r * N + c];
            db[c] = sum;
          }
        });
        return Ok();
      }
      case OpCode::MatMul: {
        AIOS_TRY(need(2, 1));
        const Tensor& A = op.inputs[0]; const Tensor& B = op.inputs[1]; const Tensor& C = op.outputs[0];
        if (A.rank() != 2 || B.rank() != 2 || C.rank() != 2) return Result<void>(Status::Invalid, "matmul needs rank-2 tensors");
        uint64_t M = A.shape[0], K = A.shape[1], N = B.shape[1];
        if (B.shape[0] != K || C.shape[0] != M || C.shape[1] != N) return Result<void>(Status::Invalid, "matmul shapes: " + A.describe() + " · " + B.describe() + " -> " + C.describe());
        if (A.dtype != B.dtype || A.dtype != C.dtype) return Result<void>(Status::Invalid, "matmul dtype mismatch");
        if (A.dtype == DType::F32) matmul<float>(at<float>(in[0], A), at<float>(in[1], B), at<float>(out[0], C), M, K, N, lanes);
        else if (A.dtype == DType::F64) matmul<double>(at<double>(in[0], A), at<double>(in[1], B), at<double>(out[0], C), M, K, N, lanes);
        else return Result<void>(Status::Unsupported, "matmul supports f32/f64");
        return Ok();
      }
      case OpCode::MatMulGradA: {
        AIOS_TRY(need(2, 1));
        // dA[MxK] = dC[MxN] · B[KxN]^T
        const Tensor& dC = op.inputs[0]; const Tensor& B = op.inputs[1]; const Tensor& dA = op.outputs[0];
        if (dC.rank() != 2 || B.rank() != 2 || dA.rank() != 2) return Result<void>(Status::Invalid, "matmul-grad-a needs rank-2 tensors");
        uint64_t M = dC.shape[0], N = dC.shape[1], K = B.shape[0];
        if (B.shape[1] != N || dA.shape[0] != M || dA.shape[1] != K) return Result<void>(Status::Invalid, "matmul-grad-a shape mismatch");
        const float* dc_ptr = at<float>(in[0], dC);
        const float* b_ptr = at<float>(in[1], B);
        float* da_ptr = at<float>(out[0], dA);
        parallel_for(M, lanes, [&](uint64_t r0, uint64_t r1) {
          for (uint64_t r = r0; r < r1; ++r) {
            for (uint64_t k = 0; k < K; ++k) {
              float acc = 0.0f;
              for (uint64_t n = 0; n < N; ++n) acc += dc_ptr[r * N + n] * b_ptr[k * N + n];
              da_ptr[r * K + k] = acc;
            }
          }
        });
        return Ok();
      }
      case OpCode::MatMulGradB: {
        AIOS_TRY(need(2, 1));
        // dB[KxN] = A[MxK]^T · dC[MxN]
        const Tensor& A = op.inputs[0]; const Tensor& dC = op.inputs[1]; const Tensor& dB = op.outputs[0];
        if (A.rank() != 2 || dC.rank() != 2 || dB.rank() != 2) return Result<void>(Status::Invalid, "matmul-grad-b needs rank-2 tensors");
        uint64_t M = A.shape[0], K = A.shape[1], N = dC.shape[1];
        if (dC.shape[0] != M || dB.shape[0] != K || dB.shape[1] != N) return Result<void>(Status::Invalid, "matmul-grad-b shape mismatch");
        const float* a_ptr = at<float>(in[0], A);
        const float* dc_ptr = at<float>(in[1], dC);
        float* db_ptr = at<float>(out[0], dB);
        parallel_for(K, lanes, [&](uint64_t k0, uint64_t k1) {
          for (uint64_t k = k0; k < k1; ++k) {
            for (uint64_t n = 0; n < N; ++n) {
              float acc = 0.0f;
              for (uint64_t m = 0; m < M; ++m) acc += a_ptr[m * K + k] * dc_ptr[m * N + n];
              db_ptr[k * N + n] = acc;
            }
          }
        });
        return Ok();
      }
      case OpCode::LayerNorm: {
        AIOS_TRY(need(3, 1));
        const Tensor& X = op.inputs[0]; const Tensor& G = op.inputs[1]; const Tensor& B = op.inputs[2];
        const Tensor& Y = op.outputs[0];
        uint64_t N = G.elements();
        if (N == 0 || B.elements() != N || X.elements() != Y.elements()) return Result<void>(Status::Invalid, "layernorm shape mismatch");
        uint64_t M = X.elements() / N;
        const float* x = at<float>(in[0], X);
        const float* g = at<float>(in[1], G);
        const float* b = at<float>(in[2], B);
        float* y = at<float>(out[0], Y);
        float* mu_out = op.outputs.size() > 1 ? at<float>(out[1], op.outputs[1]) : nullptr;
        float* inv_out = op.outputs.size() > 2 ? at<float>(out[2], op.outputs[2]) : nullptr;
        parallel_for(M, lanes, [&](uint64_t r0, uint64_t r1) {
          for (uint64_t r = r0; r < r1; ++r) {
            float mu = 0.0f;
            for (uint64_t c = 0; c < N; ++c) mu += x[r * N + c];
            mu /= (float)N;
            float var = 0.0f;
            for (uint64_t c = 0; c < N; ++c) { float d = x[r * N + c] - mu; var += d * d; }
            var /= (float)N;
            float inv = 1.0f / std::sqrt(var + 1e-5f);
            if (mu_out) mu_out[r] = mu;
            if (inv_out) inv_out[r] = inv;
            for (uint64_t c = 0; c < N; ++c) {
              y[r * N + c] = g[c] * (x[r * N + c] - mu) * inv + b[c];
            }
          }
        });
        return Ok();
      }
      case OpCode::LayerNormGrad: {
        AIOS_TRY(need(3, 3));
        const Tensor& dY = op.inputs[0]; const Tensor& X = op.inputs[1]; const Tensor& G = op.inputs[2];
        const Tensor& dX = op.outputs[0]; const Tensor& dG = op.outputs[1]; const Tensor& dB = op.outputs[2];
        uint64_t N = G.elements();
        if (N == 0 || dY.elements() != X.elements() || dX.elements() != X.elements() || dG.elements() != N || dB.elements() != N) {
          return Result<void>(Status::Invalid, "layernorm-grad shape mismatch");
        }
        uint64_t M = X.elements() / N;
        const float* dy = at<float>(in[0], dY);
        const float* x = at<float>(in[1], X);
        const float* g = at<float>(in[2], G);
        float* dx = at<float>(out[0], dX);
        float* dg = at<float>(out[1], dG);
        float* db = at<float>(out[2], dB);
        std::fill(dg, dg + N, 0.0f);
        std::fill(db, db + N, 0.0f);
        for (uint64_t r = 0; r < M; ++r) {
          float mu = 0.0f;
          for (uint64_t c = 0; c < N; ++c) mu += x[r * N + c];
          mu /= (float)N;
          float var = 0.0f;
          for (uint64_t c = 0; c < N; ++c) { float d = x[r * N + c] - mu; var += d * d; }
          var /= (float)N;
          float inv = 1.0f / std::sqrt(var + 1e-5f);
          float sum_d = 0.0f, sum_dn = 0.0f;
          for (uint64_t c = 0; c < N; ++c) {
            float xh = (x[r * N + c] - mu) * inv;
            float d = dy[r * N + c];
            dg[c] += d * xh;
            db[c] += d;
            float dn = d * g[c];
            sum_d += dn;
            sum_dn += dn * xh;
          }
          for (uint64_t c = 0; c < N; ++c) {
            float xh = (x[r * N + c] - mu) * inv;
            float dn = dy[r * N + c] * g[c];
            dx[r * N + c] = inv * (dn - (sum_d + xh * sum_dn) / (float)N);
          }
        }
        return Ok();
      }
      case OpCode::SoftmaxCrossEntropyLoss: {
        AIOS_TRY(need(2, 2));
        const Tensor& Logits = op.inputs[0]; const Tensor& Targets = op.inputs[1];
        const Tensor& Loss = op.outputs[0]; const Tensor& Probs = op.outputs[1];
        uint64_t T = Targets.elements();
        if (T == 0) return Result<void>(Status::Invalid, "empty targets");
        uint64_t V = Logits.elements() / T;
        const float* logits = at<float>(in[0], Logits);
        const float* tgt_f = at<float>(in[1], Targets);
        float* loss = at<float>(out[0], Loss);
        float* probs = at<float>(out[1], Probs);
        float total_loss = 0.0f;
        int counted = 0;
        for (uint64_t t = 0; t < T; ++t) {
          float best = -1e30f;
          for (uint64_t w = 0; w < V; ++w) {
            float val = logits[t * V + w];
            probs[t * V + w] = val;
            if (val > best) best = val;
          }
          float sum = 0.0f;
          for (uint64_t w = 0; w < V; ++w) {
            float e = std::exp(probs[t * V + w] - best);
            probs[t * V + w] = e;
            sum += e;
          }
          float inv = sum > 0.0f ? 1.0f / sum : 0.0f;
          for (uint64_t w = 0; w < V; ++w) probs[t * V + w] *= inv;
          int tgt = (int)tgt_f[t];
          if (tgt >= 0 && tgt < (int)V) {
            float p = probs[t * V + tgt];
            total_loss += -std::log(p > 1e-12f ? p : 1e-12f);
            ++counted;
          }
        }
        *loss = counted > 0 ? (total_loss / (float)counted) : 0.0f;
        return Ok();
      }
      case OpCode::SoftmaxCrossEntropyGrad: {
        AIOS_TRY(need(2, 1));
        const Tensor& Probs = op.inputs[0]; const Tensor& Targets = op.inputs[1];
        const Tensor& dLogits = op.outputs[0];
        uint64_t T = Targets.elements();
        if (T == 0) return Result<void>(Status::Invalid, "empty targets");
        uint64_t V = Probs.elements() / T;
        const float* probs = at<float>(in[0], Probs);
        const float* tgt_f = at<float>(in[1], Targets);
        float* dl = at<float>(out[0], dLogits);
        // The loss is the MEAN over the targets that counted, so its gradient
        // is scaled by 1/count — 1/(T-1) was the caller's convention leaking in
        // and made every gradient here disagree with the loss above it.
        uint64_t counted = 0;
        for (uint64_t t = 0; t < T; ++t) { int tgt = (int)tgt_f[t]; if (tgt >= 0 && tgt < (int)V) ++counted; }
        float norm = counted ? 1.0f / (float)counted : 0.0f;
        for (uint64_t t = 0; t < T; ++t) {
          int tgt = (int)tgt_f[t];
          bool valid = tgt >= 0 && tgt < (int)V;
          for (uint64_t w = 0; w < V; ++w) {
            float d = valid ? probs[t * V + w] : 0.0f;
            if (valid && (int)w == tgt) d -= 1.0f;
            dl[t * V + w] = d * norm;
          }
        }
        return Ok();
      }
      case OpCode::CausalAttention: {
        AIOS_TRY(need(3, 2));
        const Tensor& Q = op.inputs[0]; const Tensor& K = op.inputs[1]; const Tensor& Vt = op.inputs[2];
        const Tensor& O = op.outputs[0]; const Tensor& Att = op.outputs[1];
        if (Q.rank() != 2 || K.elements() != Q.elements() || Vt.elements() != Q.elements() || O.elements() != Q.elements()) return Result<void>(Status::Invalid, "causal-attention: q, k, v, o must be [TxD]");
        uint64_t T = Q.shape[0], D = Q.shape[1];
        uint64_t H = op.scalar >= 1.0 ? (uint64_t)op.scalar : 1;
        if (D % H != 0 || Att.elements() != H * T * T) return Result<void>(Status::Invalid, "causal-attention: heads must divide D and att must be [HxTxT]");
        uint64_t dh = D / H;
        const float scale = 1.0f / std::sqrt((float)dh);
        const float* q = at<float>(in[0], Q); const float* k = at<float>(in[1], K); const float* v = at<float>(in[2], Vt);
        float* o = at<float>(out[0], O); float* att = at<float>(out[1], Att);
        parallel_for(H * T, lanes, [&](uint64_t a, uint64_t b) {
          for (uint64_t idx = a; idx < b; ++idx) {
            uint64_t h = idx / T, t = idx % T;
            float* row = att + (h * T + t) * T;
            float best = -1e30f;
            for (uint64_t s2 = 0; s2 <= t; ++s2) {
              float dot = 0.0f;
              for (uint64_t j = 0; j < dh; ++j) dot += q[t * D + h * dh + j] * k[s2 * D + h * dh + j];
              row[s2] = dot * scale;
              if (row[s2] > best) best = row[s2];
            }
            for (uint64_t s2 = t + 1; s2 < T; ++s2) row[s2] = 0.0f;
            float sum = 0.0f;
            for (uint64_t s2 = 0; s2 <= t; ++s2) { row[s2] = std::exp(row[s2] - best); sum += row[s2]; }
            float inv = sum > 0.0f ? 1.0f / sum : 0.0f;
            for (uint64_t s2 = 0; s2 <= t; ++s2) row[s2] *= inv;
            for (uint64_t j = 0; j < dh; ++j) {
              float acc = 0.0f;
              for (uint64_t s2 = 0; s2 <= t; ++s2) acc += row[s2] * v[s2 * D + h * dh + j];
              o[t * D + h * dh + j] = acc;
            }
          }
        });
        return Ok();
      }
      case OpCode::CausalAttentionGrad: {
        AIOS_TRY(need(5, 3));
        const Tensor& dO = op.inputs[0]; const Tensor& Q = op.inputs[1]; const Tensor& K = op.inputs[2];
        const Tensor& Vt = op.inputs[3]; const Tensor& Att = op.inputs[4];
        const Tensor& dQ = op.outputs[0]; const Tensor& dK = op.outputs[1]; const Tensor& dV = op.outputs[2];
        if (Q.rank() != 2) return Result<void>(Status::Invalid, "causal-attention-grad: q must be [TxD]");
        uint64_t T = Q.shape[0], D = Q.shape[1];
        uint64_t H = op.scalar >= 1.0 ? (uint64_t)op.scalar : 1;
        if (D % H != 0 || Att.elements() != H * T * T || dO.elements() != T * D || K.elements() != T * D || Vt.elements() != T * D ||
            dQ.elements() != T * D || dK.elements() != T * D || dV.elements() != T * D) {
          return Result<void>(Status::Invalid, "causal-attention-grad shape mismatch");
        }
        uint64_t dh = D / H;
        const float scale = 1.0f / std::sqrt((float)dh);
        const float* dout = at<float>(in[0], dO); const float* q = at<float>(in[1], Q); const float* k = at<float>(in[2], K);
        const float* v = at<float>(in[3], Vt); const float* att = at<float>(in[4], Att);
        float* dq = at<float>(out[0], dQ); float* dk = at<float>(out[1], dK); float* dv = at<float>(out[2], dV);
        // Phase 1, per (head, query row): ds = softmax-jacobian(dA) * scale, and dq from it.
        std::vector<float> ds(H * T * T, 0.0f);
        parallel_for(H * T, lanes, [&](uint64_t a, uint64_t b) {
          for (uint64_t idx = a; idx < b; ++idx) {
            uint64_t h = idx / T, t = idx % T;
            const float* arow = att + (h * T + t) * T;
            float* drow = ds.data() + (h * T + t) * T;
            float dot = 0.0f;
            for (uint64_t s2 = 0; s2 <= t; ++s2) {
              float dA = 0.0f;
              for (uint64_t j = 0; j < dh; ++j) dA += dout[t * D + h * dh + j] * v[s2 * D + h * dh + j];
              drow[s2] = dA;
              dot += arow[s2] * dA;
            }
            for (uint64_t j = 0; j < dh; ++j) dq[t * D + h * dh + j] = 0.0f;
            for (uint64_t s2 = 0; s2 <= t; ++s2) {
              float d = arow[s2] * (drow[s2] - dot) * scale;
              drow[s2] = d;
              for (uint64_t j = 0; j < dh; ++j) dq[t * D + h * dh + j] += d * k[s2 * D + h * dh + j];
            }
          }
        });
        // Phase 2, per (head, key row): dk and dv gather over the queries that saw this key.
        parallel_for(H * T, lanes, [&](uint64_t a, uint64_t b) {
          for (uint64_t idx = a; idx < b; ++idx) {
            uint64_t h = idx / T, s2 = idx % T;
            for (uint64_t j = 0; j < dh; ++j) {
              float ak = 0.0f, av = 0.0f;
              for (uint64_t t = s2; t < T; ++t) {
                ak += ds[(h * T + t) * T + s2] * q[t * D + h * dh + j];
                av += att[(h * T + t) * T + s2] * dout[t * D + h * dh + j];
              }
              dk[s2 * D + h * dh + j] = ak;
              dv[s2 * D + h * dh + j] = av;
            }
          }
        });
        return Ok();
      }
      case OpCode::AdamStep: {
        AIOS_TRY(need(4, 1));
        float* w = at<float>(out[0], op.outputs[0]);
        const float* grad = at<float>(in[1], op.inputs[1]);
        float* m = at<float>(in[2], op.inputs[2]);
        float* v = at<float>(in[3], op.inputs[3]);
        uint64_t n = op.outputs[0].elements();
        // the rate as given: 0 is a step that moves nothing (it used to become
        // 1e-3, so `lm train N 0` - the engines' agreement check - trained)
        float lr = op.scalar > 0.0 ? (float)op.scalar : 0.0f;
        float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f, decay = 0.01f;
        parallel_for(n, lanes, [&](uint64_t a, uint64_t b) {
          for (uint64_t i = a; i < b; ++i) {
            float g = grad[i];
            m[i] = b1 * m[i] + (1.0f - b1) * g;
            v[i] = b2 * v[i] + (1.0f - b2) * g * g;
            w[i] -= lr * (m[i] / (std::sqrt(v[i]) + eps) + decay * w[i]);
          }
        });
        return Ok();
      }
      case OpCode::ReduceSum: {
        AIOS_TRY(need(1, 1));
        if (op.inputs[0].dtype != DType::F32 || op.outputs[0].dtype != DType::F32) return Result<void>(Status::Unsupported, "reduce-sum supports f32");
        const float* x = at<float>(in[0], op.inputs[0]);
        uint64_t n = op.inputs[0].elements();
        std::vector<double> partial(std::max<uint32_t>(1, lanes), 0.0);
        std::atomic<uint32_t> idx{0};
        parallel_for(n, lanes, [&](uint64_t a, uint64_t b) { double s = 0; for (uint64_t i = a; i < b; ++i) s += x[i]; partial[idx++] = s; });
        double s = 0; for (double p : partial) s += p;
        *at<float>(out[0], op.outputs[0]) = (float)s;
        return Ok();
      }
      case OpCode::Softmax: case OpCode::Normalize: {
        AIOS_TRY(need(1, 1));
        if (op.inputs[0].dtype != DType::F32 || op.outputs[0].dtype != DType::F32) return Result<void>(Status::Unsupported, "softmax/normalize support f32");
        const Tensor& X = op.inputs[0];
        if (X.elements() != op.outputs[0].elements()) return Result<void>(Status::Invalid, "shape mismatch");
        uint64_t last = X.shape.empty() ? 0 : X.shape.back();
        if (last == 0) return Ok();
        uint64_t rows = X.elements() / last;
        const float* x = at<float>(in[0], X); float* o = at<float>(out[0], op.outputs[0]);
        bool soft = op.code == OpCode::Softmax;
        parallel_for(rows, lanes, [&](uint64_t r0, uint64_t r1) {
          for (uint64_t r = r0; r < r1; ++r) {
            const float* xr = x + r * last; float* orow = o + r * last;
            if (soft) {
              float m = xr[0]; for (uint64_t i = 1; i < last; ++i) m = std::max(m, xr[i]);
              double s = 0; for (uint64_t i = 0; i < last; ++i) { orow[i] = std::exp(xr[i] - m); s += orow[i]; }
              for (uint64_t i = 0; i < last; ++i) orow[i] = (float)(orow[i] / s);
            } else {
              double s = 0; for (uint64_t i = 0; i < last; ++i) s += xr[i];
              for (uint64_t i = 0; i < last; ++i) orow[i] = s != 0 ? (float)(xr[i] / s) : 0.f;
            }
          }
        });
        return Ok();
      }
      case OpCode::Histogram: {
        AIOS_TRY(need(1, 1));
        if (op.inputs[0].dtype != DType::U8 || op.outputs[0].dtype != DType::F32 || op.outputs[0].elements() < 256) return Result<void>(Status::Invalid, "histogram: u8 input, f32[256] output");
        const uint8_t* x = at<uint8_t>(in[0], op.inputs[0]); float* o = at<float>(out[0], op.outputs[0]);
        std::vector<std::vector<uint64_t>> parts(std::max<uint32_t>(1, lanes), std::vector<uint64_t>(256, 0));
        std::atomic<uint32_t> idx{0};
        parallel_for(op.inputs[0].elements(), lanes, [&](uint64_t a, uint64_t b) { auto& h = parts[idx++]; for (uint64_t i = a; i < b; ++i) h[x[i]]++; });
        for (int i = 0; i < 256; ++i) { uint64_t s = 0; for (auto& h : parts) s += h[i]; o[i] = (float)s; }
        return Ok();
      }
      case OpCode::Custom: {
        CustomKernel k;
        { hal::LockGuard g(mu_); auto it = kernels_.find(op.custom_kernel); if (it == kernels_.end()) return Result<void>(Status::Unsupported, "no such custom kernel on this device"); k = it->second; }
        KernelArgs args{op, in, out, lanes};
        return k(args);
      }
    }
    return Result<void>(Status::Unsupported, std::string("opcode ") + opcode_name(op.code));
  }

  Result<void> dispatch_elementwise(const Operation& op, std::vector<MemoryFabric::Window>& in, std::vector<MemoryFabric::Window>& out, uint32_t lanes) {
    for (auto& t : op.inputs) if (t.dtype != op.outputs[0].dtype) return Result<void>(Status::Invalid, "dtype mismatch");
    switch (op.outputs[0].dtype) {
      case DType::F32: return elementwise<float>(op.code, op, in, out, lanes);
      case DType::F64: return elementwise<double>(op.code, op, in, out, lanes);
      case DType::I32: return elementwise<int32_t>(op.code, op, in, out, lanes);
      case DType::I64: return elementwise<int64_t>(op.code, op, in, out, lanes);
      case DType::U8: return elementwise<uint8_t>(op.code, op, in, out, lanes);
    }
    return Result<void>(Status::Unsupported);
  }

  DeviceID id_;
  std::string name_;
  DeviceCapacity cap_;
  hal::Host* host_ = nullptr;
  std::atomic<uint32_t> busy_{0};
  mutable hal::Mutex mu_;
  std::unordered_map<uint64_t, CustomKernel> kernels_;
};

}  // namespace

std::unique_ptr<Device> make_cpu_device(const DeviceID& id, uint32_t lanes, uint64_t memory, hal::Host* host) {
  return std::make_unique<CPUDevice>(id, lanes, memory, host);
}

void DeviceFabric::add(std::unique_ptr<Device> d) { hal::LockGuard g(mu_); devices_.push_back(std::move(d)); }
void DeviceFabric::set_lazy_loader(std::function<void()> loader) {
  hal::LockGuard g(load_mu_);
  loader_ = std::move(loader);
  loaded_.store(!loader_, std::memory_order_release);
}
void DeviceFabric::ensure_loaded() {
  if (loaded_.load(std::memory_order_acquire)) return;
  // The loader registers devices through add(); a query it makes itself must not re-enter it.
  static thread_local bool in_loader = false;
  if (in_loader) return;
  hal::LockGuard g(load_mu_);
  if (loaded_.load(std::memory_order_acquire)) return;
  in_loader = true;
  auto fn = std::move(loader_);
  loader_ = nullptr;
  if (fn) fn();
  in_loader = false;
  loaded_.store(true, std::memory_order_release);
}
Device* DeviceFabric::get(const DeviceID& id) {
  for (int pass = 0; pass < 2; ++pass) {
    {
      hal::LockGuard g(mu_);
      for (auto& d : devices_) if (d->id() == id) return d.get();
    }
    if (pass == 0) ensure_loaded();
  }
  return nullptr;
}
std::vector<Device*> DeviceFabric::all_loaded() { hal::LockGuard g(mu_); std::vector<Device*> v; for (auto& d : devices_) v.push_back(d.get()); return v; }
std::vector<Device*> DeviceFabric::all() { ensure_loaded(); return all_loaded(); }
std::vector<Device*> DeviceFabric::of_type(DeviceType t) {
  if (t != DeviceType::CPU) ensure_loaded();
  hal::LockGuard g(mu_); std::vector<Device*> v; for (auto& d : devices_) if (t == DeviceType::Any || d->type() == t) v.push_back(d.get()); return v;
}
size_t DeviceFabric::count() { ensure_loaded(); hal::LockGuard g(mu_); return devices_.size(); }

}  // namespace aios
