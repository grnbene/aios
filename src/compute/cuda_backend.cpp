// The CUDA backend, on the GPU.
//
// What was here before reported cap_.lanes = 2048 and 2 TFLOP/s as struct
// literals on every machine — including one with no NVIDIA card in it — and
// then ran single-threaded `for` loops over system memory. There was no
// libcuda, no context, no PTX and no device.
//
// This binds the CUDA DRIVER API at runtime through the host boundary, so a
// machine without the driver simply has no CUDA device (which is not the same
// thing as having a slow one). The kernels are compiled from source by NVRTC
// on the machine that will run them, so there is no pre-built cubin to go
// stale against a card nobody predicted.
#include "aios/compute/backends.hpp"
#include "aios/compute/gpu_telemetry.hpp"
#include "aios/hal/host.hpp"

#include <algorithm>
#include <cstring>
#include <map>

namespace aios::compute {
namespace {

using CUresult = int;
using CUdevice = int;
using CUdeviceptr = unsigned long long;
struct CUctx_st;  using CUcontext = CUctx_st*;
struct CUmod_st;  using CUmodule = CUmod_st*;
struct CUfunc_st; using CUfunction = CUfunc_st*;
struct _nvrtcProgram; using nvrtcProgram = _nvrtcProgram*;

constexpr CUresult CUDA_SUCCESS_ = 0;

// The attribute numbers are the driver's own, from cuda.h; they are ABI, not
// a guess, and are spelled here because nothing links against the SDK.
constexpr int ATTR_MULTIPROCESSOR_COUNT = 16;
constexpr int ATTR_CLOCK_RATE = 13;               // kHz
constexpr int ATTR_WARP_SIZE = 10;
constexpr int ATTR_MAX_THREADS_PER_BLOCK = 1;
constexpr int ATTR_COMPUTE_CAPABILITY_MAJOR = 75;
constexpr int ATTR_COMPUTE_CAPABILITY_MINOR = 76;

// CUDA cores per streaming multiprocessor, by compute capability. Published
// hardware, and the only way to turn an SM count into a lane count that means
// anything. Anything unrecognised falls back to the smallest real value rather
// than to a flattering one.
uint32_t cores_per_sm(int major, int minor) {
  switch (major * 10 + minor) {
    case 30: case 32: case 35: case 37: return 192;   // Kepler
    case 50: case 52: case 53: return 128;            // Maxwell
    case 60: return 64;                               // Pascal GP100
    case 61: case 62: return 128;                     // Pascal GP10x
    case 70: case 72: return 64;                      // Volta
    case 75: return 64;                               // Turing
    case 80: return 64;                               // Ampere A100
    case 86: case 87: case 89: return 128;            // Ampere GA10x / Ada
    case 90: return 128;                              // Hopper
    default: return 64;
  }
}

struct CudaApi {
  void* driver = nullptr;
  void* rtc = nullptr;
  bool bound = false;
  std::string why;

  CUresult (*Init)(unsigned) = nullptr;
  CUresult (*DeviceGetCount)(int*) = nullptr;
  CUresult (*DeviceGet)(CUdevice*, int) = nullptr;
  CUresult (*DeviceGetName)(char*, int, CUdevice) = nullptr;
  CUresult (*DeviceTotalMem)(size_t*, CUdevice) = nullptr;
  CUresult (*DeviceGetAttribute)(int*, int, CUdevice) = nullptr;
  CUresult (*DeviceGetPCIBusId)(char*, int, CUdevice) = nullptr;
  CUresult (*CtxCreate)(CUcontext*, unsigned, CUdevice) = nullptr;
  CUresult (*CtxDestroy)(CUcontext) = nullptr;
  CUresult (*CtxSetCurrent)(CUcontext) = nullptr;
  CUresult (*CtxSynchronize)() = nullptr;
  CUresult (*ModuleLoadData)(CUmodule*, const void*) = nullptr;
  CUresult (*ModuleGetFunction)(CUfunction*, CUmodule, const char*) = nullptr;
  CUresult (*MemAlloc)(CUdeviceptr*, size_t) = nullptr;
  CUresult (*MemFree)(CUdeviceptr) = nullptr;
  CUresult (*MemGetInfo)(size_t*, size_t*) = nullptr;
  CUresult (*MemcpyHtoD)(CUdeviceptr, const void*, size_t) = nullptr;
  CUresult (*MemcpyDtoH)(void*, CUdeviceptr, size_t) = nullptr;
  CUresult (*LaunchKernel)(CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                           unsigned, void*, void**, void**) = nullptr;
  CUresult (*GetErrorString)(CUresult, const char**) = nullptr;

  int (*nvrtcCreateProgram)(nvrtcProgram*, const char*, const char*, int, const char**, const char**) = nullptr;
  int (*nvrtcCompileProgram)(nvrtcProgram, int, const char**) = nullptr;
  int (*nvrtcGetPTXSize)(nvrtcProgram, size_t*) = nullptr;
  int (*nvrtcGetPTX)(nvrtcProgram, char*) = nullptr;
  int (*nvrtcGetProgramLogSize)(nvrtcProgram, size_t*) = nullptr;
  int (*nvrtcGetProgramLog)(nvrtcProgram, char*) = nullptr;
  int (*nvrtcDestroyProgram)(nvrtcProgram*) = nullptr;

  template <typename Fn>
  bool bind(void* lib, Fn& slot, const char* name) {
    slot = reinterpret_cast<Fn>(hal::library_symbol(lib, name));
    return slot != nullptr;
  }

  bool load() {
    if (bound || !why.empty()) return bound;
    for (const char* n : {"libcuda.so.1", "libcuda.so", "nvcuda.dll"}) {
      driver = hal::load_library(n);
      if (driver) break;
    }
    if (!driver) { why = "no CUDA driver library on this machine"; return false; }
    for (const char* n : {"libnvrtc.so.12", "libnvrtc.so.11", "libnvrtc.so", "nvrtc64_120_0.dll"}) {
      rtc = hal::load_library(n);
      if (rtc) break;
    }
    if (!rtc) { why = "the CUDA driver is here but NVRTC is not, so no kernel can be compiled for it"; return false; }

    bool ok = true;
    ok &= bind(driver, Init, "cuInit");
    ok &= bind(driver, DeviceGetCount, "cuDeviceGetCount");
    ok &= bind(driver, DeviceGet, "cuDeviceGet");
    ok &= bind(driver, DeviceGetName, "cuDeviceGetName");
    ok &= bind(driver, DeviceTotalMem, "cuDeviceTotalMem_v2");
    ok &= bind(driver, DeviceGetAttribute, "cuDeviceGetAttribute");
    bind(driver, DeviceGetPCIBusId, "cuDeviceGetPCIBusId");   // optional: only feeds telemetry
    ok &= bind(driver, CtxCreate, "cuCtxCreate_v2");
    ok &= bind(driver, CtxDestroy, "cuCtxDestroy_v2");
    ok &= bind(driver, CtxSetCurrent, "cuCtxSetCurrent");
    ok &= bind(driver, CtxSynchronize, "cuCtxSynchronize");
    ok &= bind(driver, ModuleLoadData, "cuModuleLoadData");
    ok &= bind(driver, ModuleGetFunction, "cuModuleGetFunction");
    ok &= bind(driver, MemAlloc, "cuMemAlloc_v2");
    ok &= bind(driver, MemFree, "cuMemFree_v2");
    if (!bind(driver, MemGetInfo, "cuMemGetInfo_v2")) bind(driver, MemGetInfo, "cuMemGetInfo");
    ok &= bind(driver, MemcpyHtoD, "cuMemcpyHtoD_v2");
    ok &= bind(driver, MemcpyDtoH, "cuMemcpyDtoH_v2");
    ok &= bind(driver, LaunchKernel, "cuLaunchKernel");
    bind(driver, GetErrorString, "cuGetErrorString");
    ok &= bind(rtc, nvrtcCreateProgram, "nvrtcCreateProgram");
    ok &= bind(rtc, nvrtcCompileProgram, "nvrtcCompileProgram");
    ok &= bind(rtc, nvrtcGetPTXSize, "nvrtcGetPTXSize");
    ok &= bind(rtc, nvrtcGetPTX, "nvrtcGetPTX");
    ok &= bind(rtc, nvrtcGetProgramLogSize, "nvrtcGetProgramLogSize");
    ok &= bind(rtc, nvrtcGetProgramLog, "nvrtcGetProgramLog");
    ok &= bind(rtc, nvrtcDestroyProgram, "nvrtcDestroyProgram");
    if (!ok) { why = "the CUDA libraries are here but do not export the driver API this expects"; return false; }

    CUresult r = Init(0);
    if (r != CUDA_SUCCESS_) { why = "cuInit failed (" + error_text(r) + ")"; return false; }
    bound = true;
    return true;
  }

  std::string error_text(CUresult r) const {
    const char* s = nullptr;
    if (GetErrorString && GetErrorString(r, &s) == CUDA_SUCCESS_ && s) return s;
    return "CUDA error " + std::to_string(r);
  }
};

CudaApi& cuda() { static CudaApi api; return api; }

// The kernels, as CUDA C, compiled by NVRTC for whatever card is present.
const char* kKernelSource = R"CUDA(
extern "C" __global__ void aios_fill(float* out, float v, unsigned long long n) {
  unsigned long long i = blockIdx.x * (unsigned long long)blockDim.x + threadIdx.x;
  if (i < n) out[i] = v;
}
extern "C" __global__ void aios_add(const float* a, const float* b, float* out, unsigned long long n) {
  unsigned long long i = blockIdx.x * (unsigned long long)blockDim.x + threadIdx.x;
  if (i < n) out[i] = a[i] + b[i];
}
extern "C" __global__ void aios_mul(const float* a, const float* b, float* out, unsigned long long n) {
  unsigned long long i = blockIdx.x * (unsigned long long)blockDim.x + threadIdx.x;
  if (i < n) out[i] = a[i] * b[i];
}
extern "C" __global__ void aios_scale(const float* a, float v, float* out, unsigned long long n) {
  unsigned long long i = blockIdx.x * (unsigned long long)blockDim.x + threadIdx.x;
  if (i < n) out[i] = a[i] * v;
}
extern "C" __global__ void aios_relu(const float* a, float* out, unsigned long long n) {
  unsigned long long i = blockIdx.x * (unsigned long long)blockDim.x + threadIdx.x;
  if (i < n) out[i] = a[i] > 0.0f ? a[i] : 0.0f;
}
extern "C" __global__ void aios_copy(const float* a, float* out, unsigned long long n) {
  unsigned long long i = blockIdx.x * (unsigned long long)blockDim.x + threadIdx.x;
  if (i < n) out[i] = a[i];
}
extern "C" __global__ void aios_matmul(const float* A, const float* B, float* C,
                                       unsigned M, unsigned K, unsigned N) {
  unsigned row = blockIdx.y * blockDim.y + threadIdx.y;
  unsigned col = blockIdx.x * blockDim.x + threadIdx.x;
  if (row >= M || col >= N) return;
  float acc = 0.0f;
  for (unsigned k = 0; k < K; ++k) acc += A[row * K + k] * B[k * N + col];
  C[row * N + col] = acc;
}
extern "C" __global__ void aios_reduce_sum(const float* a, float* out, unsigned long long n) {
  // One block, a strided read and a shared-memory tree: the reduction is small
  // beside the elementwise work and this keeps it to a single launch.
  extern __shared__ float part[];
  unsigned t = threadIdx.x;
  float acc = 0.0f;
  for (unsigned long long i = t; i < n; i += blockDim.x) acc += a[i];
  part[t] = acc;
  __syncthreads();
  for (unsigned s = blockDim.x / 2; s > 0; s >>= 1) {
    if (t < s) part[t] += part[t + s];
    __syncthreads();
  }
  if (t == 0) out[0] = part[0];
}
extern "C" __global__ void aios_gelu(const float* x, float* out, unsigned long long n) {
  unsigned long long i = blockIdx.x * (unsigned long long)blockDim.x + threadIdx.x;
  if (i < n) {
    float xi = x[i];
    float c = 0.7978845608f;
    float inner = c * (xi + 0.044715f * xi * xi * xi);
    out[i] = 0.5f * xi * (1.0f + tanhf(inner));
  }
}
extern "C" __global__ void aios_gelu_grad(const float* dy, const float* x, float* dx, unsigned long long n) {
  unsigned long long i = blockIdx.x * (unsigned long long)blockDim.x + threadIdx.x;
  if (i < n) {
    float xi = x[i];
    float x3 = xi * xi * xi;
    float c = 0.7978845608f;
    float inner = c * (xi + 0.044715f * x3);
    float t = tanhf(inner);
    float dinner = c * (1.0f + 3.0f * 0.044715f * xi * xi);
    float dg = 0.5f * (1.0f + t) + 0.5f * xi * (1.0f - t * t) * dinner;
    dx[i] = dy[i] * dg;
  }
}
extern "C" __global__ void aios_relu_grad(const float* dy, const float* x, float* dx, unsigned long long n) {
  unsigned long long i = blockIdx.x * (unsigned long long)blockDim.x + threadIdx.x;
  if (i < n) dx[i] = x[i] > 0.0f ? dy[i] : 0.0f;
}
extern "C" __global__ void aios_matmul_grad_a(const float* dC, const float* B, float* dA,
                                             unsigned M, unsigned K, unsigned N) {
  unsigned row = blockIdx.y * blockDim.y + threadIdx.y;
  unsigned col = blockIdx.x * blockDim.x + threadIdx.x;
  if (row >= M || col >= K) return;
  float acc = 0.0f;
  for (unsigned n = 0; n < N; ++n) acc += dC[row * N + n] * B[col * N + n];
  dA[row * K + col] = acc;
}
extern "C" __global__ void aios_matmul_grad_b(const float* A, const float* dC, float* dB,
                                             unsigned M, unsigned K, unsigned N) {
  unsigned row = blockIdx.y * blockDim.y + threadIdx.y;
  unsigned col = blockIdx.x * blockDim.x + threadIdx.x;
  if (row >= K || col >= N) return;
  float acc = 0.0f;
  for (unsigned m = 0; m < M; ++m) acc += A[m * K + row] * dC[m * N + col];
  dB[row * N + col] = acc;
}
extern "C" __global__ void aios_bias_add(const float* x, const float* bias, float* out,
                                        unsigned M, unsigned N) {
  unsigned row = blockIdx.y * blockDim.y + threadIdx.y;
  unsigned col = blockIdx.x * blockDim.x + threadIdx.x;
  if (row >= M || col >= N) return;
  out[row * N + col] = x[row * N + col] + bias[col];
}
extern "C" __global__ void aios_bias_add_grad(const float* dy, float* dbias,
                                             unsigned M, unsigned N) {
  unsigned col = blockIdx.x * blockDim.x + threadIdx.x;
  if (col >= N) return;
  float sum = 0.0f;
  for (unsigned m = 0; m < M; ++m) sum += dy[m * N + col];
  dbias[col] = sum;
}
extern "C" __global__ void aios_layernorm(const float* x, const float* gamma, const float* beta,
                                         float* out, float* mu_out, float* inv_out,
                                         unsigned M, unsigned N) {
  unsigned row = blockIdx.x * blockDim.x + threadIdx.x;
  if (row >= M) return;
  float mu = 0.0f;
  for (unsigned c = 0; c < N; ++c) mu += x[row * N + c];
  mu /= (float)N;
  float var = 0.0f;
  for (unsigned c = 0; c < N; ++c) { float d = x[row * N + c] - mu; var += d * d; }
  var /= (float)N;
  float inv = rsqrtf(var + 1e-5f);
  if (mu_out) mu_out[row] = mu;
  if (inv_out) inv_out[row] = inv;
  for (unsigned c = 0; c < N; ++c) {
    out[row * N + c] = gamma[c] * (x[row * N + c] - mu) * inv + beta[c];
  }
}
extern "C" __global__ void aios_layernorm_grad(const float* dy, const float* x, const float* gamma,
                                              float* dx, float* dgamma, float* dbeta,
                                              unsigned M, unsigned N) {
  unsigned col = blockIdx.x * blockDim.x + threadIdx.x;
  if (col < N) {
    float dg = 0.0f, db = 0.0f;
    for (unsigned r = 0; r < M; ++r) {
      float mu = 0.0f;
      for (unsigned c = 0; c < N; ++c) mu += x[r * N + c];
      mu /= (float)N;
      float var = 0.0f;
      for (unsigned c = 0; c < N; ++c) { float d = x[r * N + c] - mu; var += d * d; }
      var /= (float)N;
      float inv = rsqrtf(var + 1e-5f);
      float xh = (x[r * N + col] - mu) * inv;
      dg += dy[r * N + col] * xh;
      db += dy[r * N + col];
    }
    dgamma[col] = dg;
    dbeta[col] = db;
  }
  unsigned row = blockIdx.x * blockDim.x + threadIdx.x;
  if (row < M) {
    float mu = 0.0f;
    for (unsigned c = 0; c < N; ++c) mu += x[row * N + c];
    mu /= (float)N;
    float var = 0.0f;
    for (unsigned c = 0; c < N; ++c) { float d = x[row * N + c] - mu; var += d * d; }
    var /= (float)N;
    float inv = rsqrtf(var + 1e-5f);
    float sum_d = 0.0f, sum_dn = 0.0f;
    for (unsigned c = 0; c < N; ++c) {
      float xh = (x[row * N + c] - mu) * inv;
      float d = dy[row * N + c];
      float dn = d * gamma[c];
      sum_d += dn;
      sum_dn += dn * xh;
    }
    for (unsigned c = 0; c < N; ++c) {
      float xh = (x[row * N + c] - mu) * inv;
      float dn = dy[row * N + c] * gamma[c];
      dx[row * N + c] = inv * (dn - (sum_d + xh * sum_dn) / (float)N);
    }
  }
}
extern "C" __global__ void aios_softmax_ce_loss(const float* logits, const float* targets,
                                               float* loss, float* probs,
                                               unsigned T, unsigned V) {
  unsigned t = blockIdx.x * blockDim.x + threadIdx.x;
  if (t >= T) return;
  float best = -1e30f;
  for (unsigned w = 0; w < V; ++w) {
    float val = logits[t * V + w];
    probs[t * V + w] = val;
    if (val > best) best = val;
  }
  float sum = 0.0f;
  for (unsigned w = 0; w < V; ++w) {
    float e = expf(probs[t * V + w] - best);
    probs[t * V + w] = e;
    sum += e;
  }
  float inv = sum > 0.0f ? 1.0f / sum : 0.0f;
  for (unsigned w = 0; w < V; ++w) probs[t * V + w] *= inv;
  if (t == 0) {
    float total = 0.0f;
    int cnt = 0;
    for (unsigned row = 0; row < T; ++row) {
      int tgt = (int)targets[row];
      if (tgt >= 0 && tgt < (int)V) {
        float p = probs[row * V + tgt];
        total += -logf(p > 1e-12f ? p : 1e-12f);
        cnt++;
      }
    }
    loss[0] = cnt > 0 ? (total / (float)cnt) : 0.0f;
  }
}
extern "C" __global__ void aios_softmax_ce_grad(const float* probs, const float* targets,
                                               float* dlogits, unsigned T, unsigned V) {
  unsigned row = blockIdx.y * blockDim.y + threadIdx.y;
  unsigned col = blockIdx.x * blockDim.x + threadIdx.x;
  if (row >= T || col >= V) return;
  // The loss is the mean over the targets that counted; its gradient is 1/count.
  unsigned counted = 0;
  for (unsigned r = 0; r < T; ++r) { int tg = (int)targets[r]; if (tg >= 0 && tg < (int)V) ++counted; }
  float norm = counted ? 1.0f / (float)counted : 0.0f;
  int tgt = (int)targets[row];
  bool valid = tgt >= 0 && tgt < (int)V;
  float d = valid ? probs[row * V + col] : 0.0f;
  if (valid && (int)col == tgt) d -= 1.0f;
  dlogits[row * V + col] = d * norm;
}
// Causal multi-head attention, one thread per (head, query row). att holds the
// post-softmax weights (zero above the diagonal) so the backward pass can read
// them instead of recomputing the softmax.
extern "C" __global__ void aios_causal_attention(const float* q, const float* k, const float* v,
                                                 float* o, float* att, unsigned T, unsigned D, unsigned H) {
  unsigned idx = blockIdx.x * blockDim.x + threadIdx.x;
  if (idx >= H * T) return;
  unsigned h = idx / T, t = idx % T, dh = D / H;
  float scale = rsqrtf((float)dh);
  float* row = att + ((size_t)h * T + t) * T;
  float best = -1e30f;
  for (unsigned s2 = 0; s2 <= t; ++s2) {
    float dot = 0.0f;
    for (unsigned j = 0; j < dh; ++j) dot += q[t * D + h * dh + j] * k[s2 * D + h * dh + j];
    row[s2] = dot * scale;
    if (row[s2] > best) best = row[s2];
  }
  for (unsigned s2 = t + 1; s2 < T; ++s2) row[s2] = 0.0f;
  float sum = 0.0f;
  for (unsigned s2 = 0; s2 <= t; ++s2) { row[s2] = expf(row[s2] - best); sum += row[s2]; }
  float inv = sum > 0.0f ? 1.0f / sum : 0.0f;
  for (unsigned s2 = 0; s2 <= t; ++s2) row[s2] *= inv;
  for (unsigned j = 0; j < dh; ++j) {
    float acc = 0.0f;
    for (unsigned s2 = 0; s2 <= t; ++s2) acc += row[s2] * v[s2 * D + h * dh + j];
    o[t * D + h * dh + j] = acc;
  }
}
// Backward, phase 1, per (head, query row): ds = att * (dA - <att, dA>) * scale, and dq.
extern "C" __global__ void aios_causal_attention_grad_q(const float* dO, const float* q, const float* k,
                                                        const float* v, const float* att, float* ds, float* dq,
                                                        unsigned T, unsigned D, unsigned H) {
  unsigned idx = blockIdx.x * blockDim.x + threadIdx.x;
  if (idx >= H * T) return;
  unsigned h = idx / T, t = idx % T, dh = D / H;
  float scale = rsqrtf((float)dh);
  const float* arow = att + ((size_t)h * T + t) * T;
  float* drow = ds + ((size_t)h * T + t) * T;
  float dot = 0.0f;
  for (unsigned s2 = 0; s2 <= t; ++s2) {
    float dA = 0.0f;
    for (unsigned j = 0; j < dh; ++j) dA += dO[t * D + h * dh + j] * v[s2 * D + h * dh + j];
    drow[s2] = dA;
    dot += arow[s2] * dA;
  }
  for (unsigned s2 = t + 1; s2 < T; ++s2) drow[s2] = 0.0f;
  for (unsigned j = 0; j < dh; ++j) dq[t * D + h * dh + j] = 0.0f;
  for (unsigned s2 = 0; s2 <= t; ++s2) {
    float d = arow[s2] * (drow[s2] - dot) * scale;
    drow[s2] = d;
    for (unsigned j = 0; j < dh; ++j) dq[t * D + h * dh + j] += d * k[s2 * D + h * dh + j];
  }
}
// Backward, phase 2, per (head, key row): dk and dv gathered over the queries that saw this key.
extern "C" __global__ void aios_causal_attention_grad_kv(const float* dO, const float* q, const float* att,
                                                         const float* ds, float* dk, float* dv,
                                                         unsigned T, unsigned D, unsigned H) {
  unsigned idx = blockIdx.x * blockDim.x + threadIdx.x;
  if (idx >= H * T) return;
  unsigned h = idx / T, s2 = idx % T, dh = D / H;
  for (unsigned j = 0; j < dh; ++j) {
    float ak = 0.0f, av = 0.0f;
    for (unsigned t = s2; t < T; ++t) {
      ak += ds[((size_t)h * T + t) * T + s2] * q[t * D + h * dh + j];
      av += att[((size_t)h * T + t) * T + s2] * dO[t * D + h * dh + j];
    }
    dk[s2 * D + h * dh + j] = ak;
    dv[s2 * D + h * dh + j] = av;
  }
}
extern "C" __global__ void aios_adam_step(float* w, const float* grad, float* m, float* v,
                                         float lr, float b1, float b2, float eps, float decay,
                                         unsigned long long n) {
  unsigned long long i = blockIdx.x * (unsigned long long)blockDim.x + threadIdx.x;
  if (i < n) {
    float g = grad[i];
    float mi = b1 * m[i] + (1.0f - b1) * g;
    float vi = b2 * v[i] + (1.0f - b2) * g * g;
    m[i] = mi;
    v[i] = vi;
    w[i] -= lr * (mi / (sqrtf(vi) + eps) + decay * w[i]);
  }
}
)CUDA";

}  // namespace

// The device-side state, kept out of the header so nothing above the compute
// layer sees a CUDA type.
struct CudaComputeBackend::Gpu {
  CUcontext ctx = nullptr;
  hal::Mutex synced_mu;
  std::map<MemoryID, uint64_t> synced;   // Device memory -> the generation its card copy holds
  CUmodule mod = nullptr;
  CUdevice dev = 0;
  CUfunction fill = nullptr, add = nullptr, mul = nullptr, scale = nullptr;
  CUfunction relu = nullptr, copy = nullptr, matmul = nullptr, reduce = nullptr;
  CUfunction gelu = nullptr, gelu_grad = nullptr, relu_grad = nullptr;
  CUfunction matmul_grad_a = nullptr, matmul_grad_b = nullptr;
  CUfunction bias_add = nullptr, bias_add_grad = nullptr;
  CUfunction layernorm = nullptr, layernorm_grad = nullptr;
  CUfunction softmax_ce_loss = nullptr, softmax_ce_grad = nullptr;
  CUfunction adam_step = nullptr;
  CUfunction attention = nullptr, attention_grad_q = nullptr, attention_grad_kv = nullptr;
  uint32_t max_threads = 256;
  std::string pci;   // "0000:01:00.0", the name NVML and the other runtimes share for this card
  ~Gpu() {
    if (ctx) cuda().CtxDestroy(ctx);
  }
};

namespace {

// Compiles the kernels for the card that is actually here. NVRTC targets the
// device's own compute capability, so nothing is JITed from a mismatched
// virtual architecture at launch.
Result<std::string> compile_kernels(int major, int minor) {
  auto& api = cuda();
  nvrtcProgram prog = nullptr;
  if (api.nvrtcCreateProgram(&prog, kKernelSource, "aios_kernels.cu", 0, nullptr, nullptr) != 0) {
    return Result<std::string>(Status::IoError, "NVRTC would not accept the kernel source");
  }
  std::string arch = "--gpu-architecture=compute_" + std::to_string(major) + std::to_string(minor);
  const char* opts[] = {arch.c_str()};
  int rc = api.nvrtcCompileProgram(prog, 1, opts);
  if (rc != 0) {
    std::string log;
    size_t n = 0;
    if (api.nvrtcGetProgramLogSize(prog, &n) == 0 && n > 1) {
      log.resize(n);
      api.nvrtcGetProgramLog(prog, log.data());
    }
    api.nvrtcDestroyProgram(&prog);
    return Result<std::string>(Status::IoError, "NVRTC refused the kernels: " + log.substr(0, 400));
  }
  size_t ptx_size = 0;
  if (api.nvrtcGetPTXSize(prog, &ptx_size) != 0 || ptx_size == 0) {
    api.nvrtcDestroyProgram(&prog);
    return Result<std::string>(Status::IoError, "NVRTC produced no PTX");
  }
  std::string ptx(ptx_size, '\0');
  api.nvrtcGetPTX(prog, ptx.data());
  api.nvrtcDestroyProgram(&prog);
  return ptx;
}

int device_attr(CUdevice d, int which, int fallback) {
  int v = fallback;
  if (cuda().DeviceGetAttribute(&v, which, d) != CUDA_SUCCESS_) return fallback;
  return v;
}

uint64_t fnv1a_hash(const std::string& str) {
  uint64_t hash = 14695981039346656037ULL;
  for (char c : str) { hash ^= static_cast<uint8_t>(c); hash *= 1099511628211ULL; }
  return hash;
}

// One device allocation, freed however the caller leaves.
struct DeviceBuffer {
  CUdeviceptr ptr = 0;
  ~DeviceBuffer() { if (ptr) cuda().MemFree(ptr); }
  Result<void> alloc(size_t bytes) {
    CUresult r = cuda().MemAlloc(&ptr, bytes ? bytes : 1);
    if (r != CUDA_SUCCESS_) return Result<void>(Status::Exhausted, "cuMemAlloc: " + cuda().error_text(r));
    return Ok();
  }
  Result<void> upload(const void* src, size_t bytes) {
    if (!bytes) return Ok();
    CUresult r = cuda().MemcpyHtoD(ptr, src, bytes);
    if (r != CUDA_SUCCESS_) return Result<void>(Status::IoError, "cuMemcpyHtoD: " + cuda().error_text(r));
    return Ok();
  }
  Result<void> download(void* dst, size_t bytes) const {
    if (!bytes) return Ok();
    CUresult r = cuda().MemcpyDtoH(dst, ptr, bytes);
    if (r != CUDA_SUCCESS_) return Result<void>(Status::IoError, "cuMemcpyDtoH: " + cuda().error_text(r));
    return Ok();
  }
};

// MemoryType::Device on a CUDA card: a device allocation that holds the data,
// plus the host mirror the fabric reads and writes. The kernels work on the
// device copy; it is re-uploaded only when the host mirror has moved past it
// (by generation), so data that stays on the card between operations is not
// copied each time.
char kCudaOwner;   // its address tags a Mapping whose device_ctx is a CudaDeviceMemory
struct CudaDeviceMemory {
  DeviceBuffer buf;
  CUcontext ctx = nullptr;
};

class CudaDeviceMemoryProvider final : public MemoryProvider {
 public:
  CudaDeviceMemoryProvider(hal::Host& host, std::shared_ptr<CudaComputeBackend::Gpu> gpu, uint64_t capacity)
      : host_(host), gpu_(std::move(gpu)), capacity_(capacity) {}
  MemoryType type() const override { return MemoryType::Device; }
  uint64_t capacity() const override { return capacity_; }

  Result<hal::Mapping> allocate(uint64_t size, const MemoryID&) override {
    auto m = host_.map(size, hal::MappingKind::Private);
    if (!m.ok()) return m;
    if (gpu_ && gpu_->ctx) cuda().CtxSetCurrent(gpu_->ctx);
    auto dm = std::make_unique<CudaDeviceMemory>();
    dm->ctx = gpu_ ? gpu_->ctx : nullptr;
    auto r = dm->buf.alloc(size);
    if (!r.ok()) {
      host_.unmap(*m);
      return Result<hal::Mapping>(r.status(), "device memory allocation failed: " + r.error().message);
    }
    m->device_ctx = dm.release();
    m->device_owner = &kCudaOwner;
    return m;
  }

  void release(hal::Mapping& m) override {
    if (m.device_ctx && m.device_owner == &kCudaOwner) {
      auto* dm = static_cast<CudaDeviceMemory*>(m.device_ctx);
      if (dm->ctx) cuda().CtxSetCurrent(dm->ctx);
      delete dm;
      m.device_ctx = nullptr;
      m.device_owner = nullptr;
    }
    host_.unmap(m);
  }

 private:
  hal::Host& host_;
  std::shared_ptr<CudaComputeBackend::Gpu> gpu_;
  uint64_t capacity_;
};

}  // namespace

CudaComputeBackend::CudaComputeBackend(const DeviceID& id, int ordinal) : id_(id) {
  auto& api = cuda();
  if (!api.load()) { unavailable_ = api.why; return; }

  int count = 0;
  if (api.DeviceGetCount(&count) != CUDA_SUCCESS_ || count <= 0) {
    unavailable_ = "the CUDA driver is loaded but reports no devices";
    return;
  }
  if (ordinal < 0 || ordinal >= count) {
    unavailable_ = "ordinal " + std::to_string(ordinal) + " out of range (" + std::to_string(count) + " device(s))";
    return;
  }

  auto gpu = std::make_shared<Gpu>();
  CUresult r = api.DeviceGet(&gpu->dev, ordinal);
  if (r != CUDA_SUCCESS_) { unavailable_ = "cuDeviceGet: " + api.error_text(r); return; }

  char raw_name[256] = {0};
  if (api.DeviceGetName(raw_name, sizeof raw_name - 1, gpu->dev) == CUDA_SUCCESS_ && raw_name[0]) {
    name_ = raw_name;
  } else {
    name_ = "CUDA device " + std::to_string(ordinal);
  }

  int major = device_attr(gpu->dev, ATTR_COMPUTE_CAPABILITY_MAJOR, 5);
  int minor = device_attr(gpu->dev, ATTR_COMPUTE_CAPABILITY_MINOR, 0);
  int sms = device_attr(gpu->dev, ATTR_MULTIPROCESSOR_COUNT, 1);
  int clock_khz = device_attr(gpu->dev, ATTR_CLOCK_RATE, 0);
  gpu->max_threads = (uint32_t)device_attr(gpu->dev, ATTR_MAX_THREADS_PER_BLOCK, 256);
  if (api.DeviceGetPCIBusId) {
    char pci[64] = {0};
    if (api.DeviceGetPCIBusId(pci, sizeof pci - 1, gpu->dev) == CUDA_SUCCESS_) gpu->pci = normalize_pci_bus_id(pci);
  }

  size_t total = 0;
  if (api.DeviceTotalMem(&total, gpu->dev) != CUDA_SUCCESS_) total = 0;

  // Every one of these came off the device. What used to be here was
  // cap_.lanes = 2048 and 2e12 flops, written into the source.
  uint32_t cores = cores_per_sm(major, minor);
  cap_.lanes = (uint32_t)sms * cores;
  cap_.memory = total;
  cap_.flops_per_second = (uint64_t)cap_.lanes * 2ull * (uint64_t)clock_khz * 1000ull;
  cap_.native_memory = MemoryType::Device;
  cap_.reads_host_memory = true;   // execute() uploads from the operands' host windows itself

  r = api.CtxCreate(&gpu->ctx, 0, gpu->dev);
  if (r != CUDA_SUCCESS_) { unavailable_ = "cuCtxCreate: " + api.error_text(r); return; }

  auto ptx = compile_kernels(major, minor);
  if (!ptx.ok()) {
    api.CtxDestroy(gpu->ctx);
    unavailable_ = ptx.error().message;
    return;
  }
  r = api.ModuleLoadData(&gpu->mod, ptx->c_str());
  if (r != CUDA_SUCCESS_) {
    api.CtxDestroy(gpu->ctx);
    unavailable_ = "cuModuleLoadData: " + api.error_text(r);
    return;
  }

  struct { CUfunction* slot; const char* name; } wanted[] = {
    {&gpu->fill, "aios_fill"},   {&gpu->add, "aios_add"},       {&gpu->mul, "aios_mul"},
    {&gpu->scale, "aios_scale"}, {&gpu->relu, "aios_relu"},     {&gpu->copy, "aios_copy"},
    {&gpu->matmul, "aios_matmul"}, {&gpu->reduce, "aios_reduce_sum"},
    {&gpu->gelu, "aios_gelu"}, {&gpu->gelu_grad, "aios_gelu_grad"}, {&gpu->relu_grad, "aios_relu_grad"},
    {&gpu->matmul_grad_a, "aios_matmul_grad_a"}, {&gpu->matmul_grad_b, "aios_matmul_grad_b"},
    {&gpu->bias_add, "aios_bias_add"}, {&gpu->bias_add_grad, "aios_bias_add_grad"},
    {&gpu->layernorm, "aios_layernorm"}, {&gpu->layernorm_grad, "aios_layernorm_grad"},
    {&gpu->softmax_ce_loss, "aios_softmax_ce_loss"}, {&gpu->softmax_ce_grad, "aios_softmax_ce_grad"},
    {&gpu->adam_step, "aios_adam_step"},
    {&gpu->attention, "aios_causal_attention"},
    {&gpu->attention_grad_q, "aios_causal_attention_grad_q"}, {&gpu->attention_grad_kv, "aios_causal_attention_grad_kv"},
  };
  for (auto& w : wanted) {
    if (api.ModuleGetFunction(w.slot, gpu->mod, w.name) != CUDA_SUCCESS_) {
      api.CtxDestroy(gpu->ctx);
      unavailable_ = std::string("the compiled module has no ") + w.name;
      return;
    }
  }

  gpu_ = std::move(gpu);
  name_ = "cuda:" + name_ + " cc" + std::to_string(major) + "." + std::to_string(minor)
        + " [" + std::to_string(ordinal) + "]";
}

CudaComputeBackend::~CudaComputeBackend() = default;

bool CudaComputeBackend::supports(const Operation& op) const {
  if (!gpu_) return false;
  if (op.code == OpCode::Custom) {
    hal::LockGuard g(mu_);
    return kernels_.count(op.custom_kernel) > 0;
  }
  switch (op.code) {
    case OpCode::Fill: case OpCode::Add: case OpCode::Mul: case OpCode::Scale:
    case OpCode::Relu: case OpCode::Copy: case OpCode::MatMul: case OpCode::ReduceSum:
    case OpCode::Gelu: case OpCode::GeluGrad: case OpCode::ReluGrad:
    case OpCode::MatMulGradA: case OpCode::MatMulGradB:
    case OpCode::BiasAdd: case OpCode::BiasAddGrad:
    case OpCode::LayerNorm: case OpCode::LayerNormGrad:
    case OpCode::SoftmaxCrossEntropyLoss: case OpCode::SoftmaxCrossEntropyGrad:
    case OpCode::AdamStep: case OpCode::CausalAttention: case OpCode::CausalAttentionGrad:
      return true;
    default:
      return false;
  }
}

Result<void> CudaComputeBackend::register_kernel(const std::string& name, CustomKernel k) {
  hal::LockGuard g(mu_);
  kernels_[fnv1a_hash(name)] = std::move(k);
  return Ok();
}

uint32_t CudaComputeBackend::busy_lanes() const { return busy_lanes_.load(); }

Result<DeviceMemoryStats> CudaComputeBackend::memory_stats() const {
  if (!gpu_) return Result<DeviceMemoryStats>(Status::Unsupported, "no CUDA device: " + unavailable_);
  auto& api = cuda();
  if (!api.MemGetInfo) {
    DeviceMemoryStats s;
    s.total_bytes = cap_.memory;
    s.free_bytes = cap_.memory;
    s.used_bytes = 0;
    return s;
  }
  api.CtxSetCurrent(gpu_->ctx);
  size_t free_b = 0, total_b = 0;
  CUresult r = api.MemGetInfo(&free_b, &total_b);
  if (r != CUDA_SUCCESS_) return Result<DeviceMemoryStats>(Status::IoError, "cuMemGetInfo failed: " + api.error_text(r));
  DeviceMemoryStats s;
  s.total_bytes = total_b;
  s.free_bytes = free_b;
  s.used_bytes = (total_b > free_b) ? (total_b - free_b) : 0;
  s.source = "cuMemGetInfo";
  // cuMemGetInfo says how much is not free; it cannot say WHO holds it. The
  // driver's management library can, by the card's PCI address.
  if (!gpu_->pci.empty()) {
    auto nv = nvidia_memory_by_pci(gpu_->pci);
    if (nv.ok()) { s.tenants = std::move(nv->tenants); s.source = "cuMemGetInfo+nvml"; }
  }
  return s;
}

Result<void> CudaComputeBackend::execute(const Operation& op, MemoryFabric& fabric, uint32_t lanes) {
  if (!gpu_) return Result<void>(Status::Unsupported, "no CUDA device: " + unavailable_);
  auto& api = cuda();

  lanes = std::max<uint32_t>(1, std::min(lanes, cap_.lanes));
  busy_lanes_ += lanes;
  struct Guard { std::atomic<uint32_t>& b; uint32_t n; ~Guard() { b -= n; } } guard{busy_lanes_, lanes};

  std::vector<MemoryFabric::Window> in, out;
  for (const auto& t : op.inputs) {
    auto w = fabric.window(t.memory);
    if (!w.ok()) return w.error();
    if (t.offset + t.bytes() > w->size) return Result<void>(Status::Invalid, "input tensor exceeds memory");
    in.push_back(*w);
  }
  for (const auto& t : op.outputs) {
    auto w = fabric.window(t.memory);
    if (!w.ok()) return w.error();
    if (t.offset + t.bytes() > w->size) return Result<void>(Status::Invalid, "output tensor exceeds memory");
    out.push_back(*w);
  }

  if (op.code == OpCode::Custom) {
    CustomKernel k;
    {
      hal::LockGuard lg(mu_);
      auto it = kernels_.find(op.custom_kernel);
      if (it == kernels_.end()) return Result<void>(Status::NotFound, "custom kernel not registered on this device");
      k = it->second;
    }
    KernelArgs args{op, in, out, lanes};
    auto r = k(args);
    if (r.ok()) for (const auto& t : op.outputs) fabric.touch(t.memory);
    return r;
  }

  CUresult r = api.CtxSetCurrent(gpu_->ctx);
  if (r != CUDA_SUCCESS_) return Result<void>(Status::IoError, "cuCtxSetCurrent: " + api.error_text(r));

  auto host_in = [&](size_t i) { return reinterpret_cast<const float*>(in[i].base + op.inputs[i].offset); };
  auto host_out = [&](size_t i) { return reinterpret_cast<float*>(out[i].base + op.outputs[i].offset); };

  const uint64_t n_out = op.outputs.empty() ? 0 : op.outputs[0].elements();
  const uint32_t block = std::min<uint32_t>(256, gpu_->max_threads);

  auto launch1d = [&](CUfunction fn, void** args, uint64_t work) -> Result<void> {
    unsigned grid = (unsigned)((work + block - 1) / block);
    if (grid == 0) grid = 1;
    CUresult lr = api.LaunchKernel(fn, grid, 1, 1, block, 1, 1, 0, nullptr, args, nullptr);
    if (lr != CUDA_SUCCESS_) return Result<void>(Status::IoError, "cuLaunchKernel: " + api.error_text(lr));
    lr = api.CtxSynchronize();
    if (lr != CUDA_SUCCESS_) return Result<void>(Status::IoError, "the kernel faulted: " + api.error_text(lr));
    return Ok();
  };

  // Operands: CUDA Device memory is used where it lies on the card (synced
  // first if the host mirror is newer); anything else goes through a
  // temporary buffer for this call, as before.
  std::vector<std::unique_ptr<DeviceBuffer>> temps;
  std::vector<MemoryID> card_written;   // outputs written in their own card copy
  std::vector<MemoryID> written_inputs; // inputs an operation updates in place (Adam's moments)
  auto card_copy = [&](const MemoryID& id) -> CudaDeviceMemory* {
    const MemoryObject* o = fabric.lookup(id);
    if (!o || o->type != MemoryType::Device || o->mapping.device_owner != &kCudaOwner) return nullptr;
    auto* dm = static_cast<CudaDeviceMemory*>(o->mapping.device_ctx);
    return dm && dm->ctx == gpu_->ctx ? dm : nullptr;
  };
  auto sync = [&](const MemoryID& id, CudaDeviceMemory* dm) -> Result<void> {
    const MemoryObject* o = fabric.lookup(id);
    hal::LockGuard g(gpu_->synced_mu);
    auto it = gpu_->synced.find(id);
    if (it != gpu_->synced.end() && it->second == o->generation) return Ok();
    AIOS_TRY(dm->buf.upload(o->base(), o->size));
    gpu_->synced[id] = o->generation;
    return Ok();
  };
  auto bind_in = [&](size_t i, size_t bytes, CUdeviceptr* p) -> Result<void> {
    if (auto* dm = card_copy(op.inputs[i].memory)) {
      AIOS_TRY(sync(op.inputs[i].memory, dm));
      *p = dm->buf.ptr + op.inputs[i].offset;
      return Ok();
    }
    auto t = std::make_unique<DeviceBuffer>();
    AIOS_TRY(t->alloc(bytes));
    AIOS_TRY(t->upload(host_in(i), bytes));
    *p = t->ptr;
    temps.push_back(std::move(t));
    return Ok();
  };
  auto bind_out = [&](size_t i, size_t bytes, CUdeviceptr* p) -> Result<void> {
    if (auto* dm = card_copy(op.outputs[i].memory)) {
      AIOS_TRY(sync(op.outputs[i].memory, dm));   // the rest of the object must be current too
      *p = dm->buf.ptr + op.outputs[i].offset;
      return Ok();
    }
    auto t = std::make_unique<DeviceBuffer>();
    AIOS_TRY(t->alloc(bytes));
    *p = t->ptr;
    temps.push_back(std::move(t));
    return Ok();
  };
  // The result into the host mirror (what the fabric and every other device
  // read). When it was computed in the output's own card copy, that copy stays
  // current past the generation the write is about to bump.
  auto finish_to = [&](void* host, const MemoryID& id, uint64_t offset, CUdeviceptr src, size_t bytes) -> Result<void> {
    if (bytes) {
      CUresult cr = api.MemcpyDtoH(host, src, bytes);
      if (cr != CUDA_SUCCESS_) return Result<void>(Status::IoError, "cuMemcpyDtoH: " + api.error_text(cr));
    }
    if (auto* dm = card_copy(id); dm && src == dm->buf.ptr + offset) card_written.push_back(id);
    return Ok();
  };
  auto finish_out = [&](size_t i, CUdeviceptr src, size_t bytes) -> Result<void> {
    return finish_to(host_out(i), op.outputs[i].memory, op.outputs[i].offset, src, bytes);
  };

  switch (op.code) {
    case OpCode::Fill: {
      CUdeviceptr d = 0;
      AIOS_TRY(bind_out(0, n_out * sizeof(float), &d));
      float v = (float)op.scalar;
      uint64_t n = n_out;
      void* args[] = {&d, &v, &n};
      AIOS_TRY(launch1d(gpu_->fill, args, n_out));
      AIOS_TRY(finish_out(0, d, n_out * sizeof(float)));
      break;
    }
    case OpCode::Copy: case OpCode::Relu: case OpCode::Gelu: {
      if (in.empty()) return Result<void>(Status::Invalid, "this operation needs an input");
      uint64_t n = std::min(n_out, op.inputs[0].elements());
      CUdeviceptr a = 0, o = 0;
      AIOS_TRY(bind_out(0, n * sizeof(float), &o));
      AIOS_TRY(bind_in(0, n * sizeof(float), &a));
      void* args[] = {&a, &o, &n};
      CUfunction fn = (op.code == OpCode::Copy) ? gpu_->copy : ((op.code == OpCode::Relu) ? gpu_->relu : gpu_->gelu);
      AIOS_TRY(launch1d(fn, args, n));
      AIOS_TRY(finish_out(0, o, n * sizeof(float)));
      break;
    }
    case OpCode::Scale: {
      if (in.empty()) return Result<void>(Status::Invalid, "scale needs an input");
      uint64_t n = std::min(n_out, op.inputs[0].elements());
      CUdeviceptr a = 0, o = 0;
      AIOS_TRY(bind_out(0, n * sizeof(float), &o));
      AIOS_TRY(bind_in(0, n * sizeof(float), &a));
      float v = (float)op.scalar;
      void* args[] = {&a, &v, &o, &n};
      AIOS_TRY(launch1d(gpu_->scale, args, n));
      AIOS_TRY(finish_out(0, o, n * sizeof(float)));
      break;
    }
    case OpCode::Add: case OpCode::Mul: case OpCode::GeluGrad: case OpCode::ReluGrad: {
      if (in.size() < 2) return Result<void>(Status::Invalid, "this operation needs two inputs");
      uint64_t n = std::min({n_out, op.inputs[0].elements(), op.inputs[1].elements()});
      CUdeviceptr a = 0, b = 0, o = 0;
      AIOS_TRY(bind_out(0, n * sizeof(float), &o));
      AIOS_TRY(bind_in(0, n * sizeof(float), &a));
      AIOS_TRY(bind_in(1, n * sizeof(float), &b));
      void* args[] = {&a, &b, &o, &n};
      CUfunction fn = (op.code == OpCode::Add) ? gpu_->add :
                      ((op.code == OpCode::Mul) ? gpu_->mul :
                      ((op.code == OpCode::GeluGrad) ? gpu_->gelu_grad : gpu_->relu_grad));
      AIOS_TRY(launch1d(fn, args, n));
      AIOS_TRY(finish_out(0, o, n * sizeof(float)));
      break;
    }
    case OpCode::BiasAdd: {
      if (in.size() < 2) return Result<void>(Status::Invalid, "bias-add needs 2 inputs");
      unsigned N = (unsigned)op.inputs[1].elements();
      if (N == 0) return Result<void>(Status::Invalid, "invalid bias dimension");
      unsigned M = (unsigned)(op.inputs[0].elements() / N);
      CUdeviceptr x = 0, bias = 0, y = 0;
      AIOS_TRY(bind_out(0, (size_t)M * N * sizeof(float), &y));
      AIOS_TRY(bind_in(0, (size_t)M * N * sizeof(float), &x));
      AIOS_TRY(bind_in(1, (size_t)N * sizeof(float), &bias));
      void* args[] = {&x, &bias, &y, &M, &N};
      const unsigned tile = 16;
      CUresult lr = api.LaunchKernel(gpu_->bias_add, (N + tile - 1) / tile, (M + tile - 1) / tile, 1,
                                     tile, tile, 1, 0, nullptr, args, nullptr);
      if (lr != CUDA_SUCCESS_) return Result<void>(Status::IoError, "cuLaunchKernel: " + api.error_text(lr));
      lr = api.CtxSynchronize();
      if (lr != CUDA_SUCCESS_) return Result<void>(Status::IoError, "the kernel faulted: " + api.error_text(lr));
      AIOS_TRY(finish_out(0, y, (size_t)M * N * sizeof(float)));
      break;
    }
    case OpCode::BiasAddGrad: {
      if (in.empty() || out.empty()) return Result<void>(Status::Invalid, "bias-add-grad needs in and out");
      unsigned N = (unsigned)op.outputs[0].elements();
      if (N == 0) return Result<void>(Status::Invalid, "invalid bias grad dimension");
      unsigned M = (unsigned)(op.inputs[0].elements() / N);
      CUdeviceptr dy = 0, dbias = 0;
      AIOS_TRY(bind_out(0, (size_t)N * sizeof(float), &dbias));
      AIOS_TRY(bind_in(0, (size_t)M * N * sizeof(float), &dy));
      void* args[] = {&dy, &dbias, &M, &N};
      AIOS_TRY(launch1d(gpu_->bias_add_grad, args, N));
      AIOS_TRY(finish_out(0, dbias, (size_t)N * sizeof(float)));
      break;
    }
    case OpCode::ReduceSum: {
      if (in.empty()) return Result<void>(Status::Invalid, "reduce-sum needs an input");
      uint64_t n = op.inputs[0].elements();
      CUdeviceptr a = 0, o = 0;
      AIOS_TRY(bind_out(0, sizeof(float), &o));
      AIOS_TRY(bind_in(0, n * sizeof(float), &a));
      void* args[] = {&a, &o, &n};
      CUresult lr = api.LaunchKernel(gpu_->reduce, 1, 1, 1, block, 1, 1,
                                     block * sizeof(float), nullptr, args, nullptr);
      if (lr != CUDA_SUCCESS_) return Result<void>(Status::IoError, "cuLaunchKernel: " + api.error_text(lr));
      lr = api.CtxSynchronize();
      if (lr != CUDA_SUCCESS_) return Result<void>(Status::IoError, "the kernel faulted: " + api.error_text(lr));
      AIOS_TRY(finish_out(0, o, sizeof(float)));
      break;
    }
    case OpCode::MatMul: {
      if (in.size() < 2 || op.inputs[0].rank() < 2 || op.inputs[1].rank() < 2) {
        return Result<void>(Status::Invalid, "matmul needs two matrices");
      }
      unsigned M = (unsigned)op.inputs[0].shape[op.inputs[0].rank() - 2];
      unsigned K = (unsigned)op.inputs[0].shape[op.inputs[0].rank() - 1];
      unsigned N = (unsigned)op.inputs[1].shape[op.inputs[1].rank() - 1];
      if ((uint64_t)op.inputs[1].shape[op.inputs[1].rank() - 2] != K) {
        return Result<void>(Status::Invalid, "matmul inner dimensions disagree");
      }
      CUdeviceptr A = 0, B = 0, C = 0;
      AIOS_TRY(bind_out(0, (size_t)M * N * sizeof(float), &C));
      AIOS_TRY(bind_in(0, (size_t)M * K * sizeof(float), &A));
      AIOS_TRY(bind_in(1, (size_t)K * N * sizeof(float), &B));
      void* args[] = {&A, &B, &C, &M, &K, &N};
      const unsigned tile = 16;
      CUresult lr = api.LaunchKernel(gpu_->matmul, (N + tile - 1) / tile, (M + tile - 1) / tile, 1,
                                     tile, tile, 1, 0, nullptr, args, nullptr);
      if (lr != CUDA_SUCCESS_) return Result<void>(Status::IoError, "cuLaunchKernel: " + api.error_text(lr));
      lr = api.CtxSynchronize();
      if (lr != CUDA_SUCCESS_) return Result<void>(Status::IoError, "the kernel faulted: " + api.error_text(lr));
      AIOS_TRY(finish_out(0, C, (size_t)M * N * sizeof(float)));
      break;
    }
    case OpCode::MatMulGradA: {
      if (in.size() < 2) return Result<void>(Status::Invalid, "matmul-grad-a needs dC and B");
      unsigned M = (unsigned)op.inputs[0].shape[0];
      unsigned N = (unsigned)op.inputs[0].shape[1];
      unsigned K = (unsigned)op.inputs[1].shape[0];
      CUdeviceptr dC = 0, B = 0, dA = 0;
      AIOS_TRY(bind_out(0, (size_t)M * K * sizeof(float), &dA));
      AIOS_TRY(bind_in(0, (size_t)M * N * sizeof(float), &dC));
      AIOS_TRY(bind_in(1, (size_t)K * N * sizeof(float), &B));
      void* args[] = {&dC, &B, &dA, &M, &K, &N};
      const unsigned tile = 16;
      CUresult lr = api.LaunchKernel(gpu_->matmul_grad_a, (K + tile - 1) / tile, (M + tile - 1) / tile, 1,
                                     tile, tile, 1, 0, nullptr, args, nullptr);
      if (lr != CUDA_SUCCESS_) return Result<void>(Status::IoError, "cuLaunchKernel: " + api.error_text(lr));
      lr = api.CtxSynchronize();
      if (lr != CUDA_SUCCESS_) return Result<void>(Status::IoError, "the kernel faulted: " + api.error_text(lr));
      AIOS_TRY(finish_out(0, dA, (size_t)M * K * sizeof(float)));
      break;
    }
    case OpCode::MatMulGradB: {
      if (in.size() < 2) return Result<void>(Status::Invalid, "matmul-grad-b needs A and dC");
      unsigned M = (unsigned)op.inputs[0].shape[0];
      unsigned K = (unsigned)op.inputs[0].shape[1];
      unsigned N = (unsigned)op.inputs[1].shape[1];
      CUdeviceptr A = 0, dC = 0, dB = 0;
      AIOS_TRY(bind_out(0, (size_t)K * N * sizeof(float), &dB));
      AIOS_TRY(bind_in(0, (size_t)M * K * sizeof(float), &A));
      AIOS_TRY(bind_in(1, (size_t)M * N * sizeof(float), &dC));
      void* args[] = {&A, &dC, &dB, &M, &K, &N};
      const unsigned tile = 16;
      CUresult lr = api.LaunchKernel(gpu_->matmul_grad_b, (N + tile - 1) / tile, (K + tile - 1) / tile, 1,
                                     tile, tile, 1, 0, nullptr, args, nullptr);
      if (lr != CUDA_SUCCESS_) return Result<void>(Status::IoError, "cuLaunchKernel: " + api.error_text(lr));
      lr = api.CtxSynchronize();
      if (lr != CUDA_SUCCESS_) return Result<void>(Status::IoError, "the kernel faulted: " + api.error_text(lr));
      AIOS_TRY(finish_out(0, dB, (size_t)K * N * sizeof(float)));
      break;
    }
    case OpCode::LayerNorm: {
      if (in.size() < 3) return Result<void>(Status::Invalid, "layernorm needs X, gamma, beta");
      unsigned N = (unsigned)op.inputs[1].elements();
      unsigned M = (unsigned)(op.inputs[0].elements() / N);
      CUdeviceptr x = 0, gamma = 0, beta = 0, y = 0;
      DeviceBuffer mu, inv;   // scratch
      AIOS_TRY(bind_out(0, (size_t)M * N * sizeof(float), &y));
      AIOS_TRY(mu.alloc((size_t)M * sizeof(float)));
      AIOS_TRY(inv.alloc((size_t)M * sizeof(float)));
      AIOS_TRY(bind_in(0, (size_t)M * N * sizeof(float), &x));
      AIOS_TRY(bind_in(1, (size_t)N * sizeof(float), &gamma));
      AIOS_TRY(bind_in(2, (size_t)N * sizeof(float), &beta));
      void* args[] = {&x, &gamma, &beta, &y, &mu.ptr, &inv.ptr, &M, &N};
      AIOS_TRY(launch1d(gpu_->layernorm, args, M));
      AIOS_TRY(finish_out(0, y, (size_t)M * N * sizeof(float)));
      break;
    }
    case OpCode::LayerNormGrad: {
      if (in.size() < 3 || out.size() < 3) return Result<void>(Status::Invalid, "layernorm-grad needs 3 in, 3 out");
      unsigned N = (unsigned)op.inputs[2].elements();
      unsigned M = (unsigned)(op.inputs[0].elements() / N);
      CUdeviceptr dy = 0, x = 0, gamma = 0, dx = 0, dgamma = 0, dbeta = 0;
      AIOS_TRY(bind_out(0, (size_t)M * N * sizeof(float), &dx));
      AIOS_TRY(bind_out(1, (size_t)N * sizeof(float), &dgamma));
      AIOS_TRY(bind_out(2, (size_t)N * sizeof(float), &dbeta));
      AIOS_TRY(bind_in(0, (size_t)M * N * sizeof(float), &dy));
      AIOS_TRY(bind_in(1, (size_t)M * N * sizeof(float), &x));
      AIOS_TRY(bind_in(2, (size_t)N * sizeof(float), &gamma));
      void* args[] = {&dy, &x, &gamma, &dx, &dgamma, &dbeta, &M, &N};
      AIOS_TRY(launch1d(gpu_->layernorm_grad, args, std::max(M, N)));
      AIOS_TRY(finish_out(0, dx, (size_t)M * N * sizeof(float)));
      AIOS_TRY(finish_out(1, dgamma, (size_t)N * sizeof(float)));
      AIOS_TRY(finish_out(2, dbeta, (size_t)N * sizeof(float)));
      break;
    }
    case OpCode::SoftmaxCrossEntropyLoss: {
      if (in.size() < 2 || out.size() < 2) return Result<void>(Status::Invalid, "softmax-ce-loss needs 2 in, 2 out");
      unsigned T = (unsigned)op.inputs[1].elements();
      unsigned V = (unsigned)(op.inputs[0].elements() / T);
      CUdeviceptr logits = 0, targets = 0, loss = 0, probs = 0;
      AIOS_TRY(bind_out(0, sizeof(float), &loss));
      AIOS_TRY(bind_out(1, (size_t)T * V * sizeof(float), &probs));
      AIOS_TRY(bind_in(0, (size_t)T * V * sizeof(float), &logits));
      AIOS_TRY(bind_in(1, (size_t)T * sizeof(float), &targets));
      void* args[] = {&logits, &targets, &loss, &probs, &T, &V};
      AIOS_TRY(launch1d(gpu_->softmax_ce_loss, args, T));
      AIOS_TRY(finish_out(0, loss, sizeof(float)));
      AIOS_TRY(finish_out(1, probs, (size_t)T * V * sizeof(float)));
      break;
    }
    case OpCode::SoftmaxCrossEntropyGrad: {
      if (in.size() < 2 || out.empty()) return Result<void>(Status::Invalid, "softmax-ce-grad needs 2 in, 1 out");
      unsigned T = (unsigned)op.inputs[1].elements();
      unsigned V = (unsigned)(op.inputs[0].elements() / T);
      CUdeviceptr probs = 0, targets = 0, dlogits = 0;
      AIOS_TRY(bind_out(0, (size_t)T * V * sizeof(float), &dlogits));
      AIOS_TRY(bind_in(0, (size_t)T * V * sizeof(float), &probs));
      AIOS_TRY(bind_in(1, (size_t)T * sizeof(float), &targets));
      void* args[] = {&probs, &targets, &dlogits, &T, &V};
      const unsigned tile = 16;
      CUresult lr = api.LaunchKernel(gpu_->softmax_ce_grad, (V + tile - 1) / tile, (T + tile - 1) / tile, 1,
                                     tile, tile, 1, 0, nullptr, args, nullptr);
      if (lr != CUDA_SUCCESS_) return Result<void>(Status::IoError, "cuLaunchKernel: " + api.error_text(lr));
      lr = api.CtxSynchronize();
      if (lr != CUDA_SUCCESS_) return Result<void>(Status::IoError, "the kernel faulted: " + api.error_text(lr));
      AIOS_TRY(finish_out(0, dlogits, (size_t)T * V * sizeof(float)));
      break;
    }
    case OpCode::CausalAttention: {
      if (in.size() < 3 || out.size() < 2 || op.inputs[0].rank() != 2) return Result<void>(Status::Invalid, "causal-attention needs q,k,v [TxD] and o, att");
      unsigned T = (unsigned)op.inputs[0].shape[0], D = (unsigned)op.inputs[0].shape[1];
      unsigned H = op.scalar >= 1.0 ? (unsigned)op.scalar : 1;
      if (D % H != 0 || op.outputs[1].elements() != (uint64_t)H * T * T) return Result<void>(Status::Invalid, "causal-attention: heads must divide D and att must be [HxTxT]");
      size_t td = (size_t)T * D * sizeof(float), ta = (size_t)H * T * T * sizeof(float);
      CUdeviceptr q = 0, k = 0, v = 0, o = 0, att = 0;
      AIOS_TRY(bind_out(0, td, &o)); AIOS_TRY(bind_out(1, ta, &att));
      AIOS_TRY(bind_in(0, td, &q)); AIOS_TRY(bind_in(1, td, &k)); AIOS_TRY(bind_in(2, td, &v));
      void* args[] = {&q, &k, &v, &o, &att, &T, &D, &H};
      AIOS_TRY(launch1d(gpu_->attention, args, (uint64_t)H * T));
      AIOS_TRY(finish_out(0, o, td));
      AIOS_TRY(finish_out(1, att, ta));
      break;
    }
    case OpCode::CausalAttentionGrad: {
      if (in.size() < 5 || out.size() < 3 || op.inputs[1].rank() != 2) return Result<void>(Status::Invalid, "causal-attention-grad needs dO,q,k,v,att and dq,dk,dv");
      unsigned T = (unsigned)op.inputs[1].shape[0], D = (unsigned)op.inputs[1].shape[1];
      unsigned H = op.scalar >= 1.0 ? (unsigned)op.scalar : 1;
      if (D % H != 0 || op.inputs[4].elements() != (uint64_t)H * T * T) return Result<void>(Status::Invalid, "causal-attention-grad: heads must divide D and att must be [HxTxT]");
      size_t td = (size_t)T * D * sizeof(float), ta = (size_t)H * T * T * sizeof(float);
      CUdeviceptr dO = 0, q = 0, k = 0, v = 0, att = 0, dq = 0, dk = 0, dv = 0;
      DeviceBuffer ds;   // scratch
      AIOS_TRY(ds.alloc(ta)); AIOS_TRY(bind_out(0, td, &dq)); AIOS_TRY(bind_out(1, td, &dk)); AIOS_TRY(bind_out(2, td, &dv));
      AIOS_TRY(bind_in(0, td, &dO)); AIOS_TRY(bind_in(1, td, &q)); AIOS_TRY(bind_in(2, td, &k));
      AIOS_TRY(bind_in(3, td, &v)); AIOS_TRY(bind_in(4, ta, &att));
      void* args1[] = {&dO, &q, &k, &v, &att, &ds.ptr, &dq, &T, &D, &H};
      AIOS_TRY(launch1d(gpu_->attention_grad_q, args1, (uint64_t)H * T));
      void* args2[] = {&dO, &q, &att, &ds.ptr, &dk, &dv, &T, &D, &H};
      AIOS_TRY(launch1d(gpu_->attention_grad_kv, args2, (uint64_t)H * T));
      AIOS_TRY(finish_out(0, dq, td));
      AIOS_TRY(finish_out(1, dk, td));
      AIOS_TRY(finish_out(2, dv, td));
      break;
    }
    case OpCode::AdamStep: {
      if (in.size() < 4 || out.empty()) return Result<void>(Status::Invalid, "adam-step needs 4 in, 1 out");
      uint64_t n = op.outputs[0].elements();
      const size_t bytes = n * sizeof(float);
      CUdeviceptr w = 0, grad = 0, m = 0, v = 0;
      // w is read and written in place: its current values go up with it.
      AIOS_TRY(bind_out(0, bytes, &w));
      if (!card_copy(op.outputs[0].memory)) {
        CUresult ur = api.MemcpyHtoD(w, host_out(0), bytes);
        if (ur != CUDA_SUCCESS_) return Result<void>(Status::IoError, "cuMemcpyHtoD: " + api.error_text(ur));
      }
      AIOS_TRY(bind_in(1, bytes, &grad));
      AIOS_TRY(bind_in(2, bytes, &m));
      AIOS_TRY(bind_in(3, bytes, &v));
      float lr = op.scalar > 0.0 ? (float)op.scalar : 0.0f;   // as given (0 moves nothing)
      float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f, decay = 0.01f;
      void* args[] = {&w, &grad, &m, &v, &lr, &b1, &b2, &eps, &decay, &n};
      AIOS_TRY(launch1d(gpu_->adam_step, args, n));
      AIOS_TRY(finish_out(0, w, bytes));
      // The moments are inputs the kernel updates: written back, and marked
      // written so every copy of them moves on.
      AIOS_TRY(finish_to(in[2].base + op.inputs[2].offset, op.inputs[2].memory, op.inputs[2].offset, m, bytes));
      AIOS_TRY(finish_to(in[3].base + op.inputs[3].offset, op.inputs[3].memory, op.inputs[3].offset, v, bytes));
      written_inputs.push_back(op.inputs[2].memory);
      written_inputs.push_back(op.inputs[3].memory);
      if (out.size() > 1) { AIOS_TRY(finish_out(1, m, bytes)); }
      if (out.size() > 2) { AIOS_TRY(finish_out(2, v, bytes)); }
      break;
    }
    default:
      return Result<void>(Status::Unsupported,
          std::string("no CUDA kernel for ") + opcode_name(op.code) + " on this device");
  }

  for (const auto& t : op.outputs) fabric.touch(t.memory);
  for (const auto& id : written_inputs) fabric.touch(id);
  {
    hal::LockGuard g(gpu_->synced_mu);
    for (const auto& id : card_written)
      if (const MemoryObject* o = fabric.lookup(id)) gpu_->synced[id] = o->generation;
  }
  return Ok();
}

AcceleratorReport register_cuda_devices(DeviceFabric& devices, MemoryFabric& memory, hal::Host& host, IdGenerator& ids) {
  AcceleratorReport rep;
  if (const std::string e = host.env("AIOS_NO_CUDA"); !e.empty() && e != "0") {
    rep.note = "CUDA disabled (AIOS_NO_CUDA)";
    return rep;
  }
  auto& api = cuda();
  if (!api.load()) { rep.note = api.why; return rep; }
  int count = 0;
  if (api.DeviceGetCount(&count) != CUDA_SUCCESS_ || count <= 0) {
    rep.note = "the CUDA driver is loaded but reports no devices";
    return rep;
  }
  // One Device per physical ordinal — a box with several identical cards (the
  // common case, not the exception) used to register only ordinal 0 and leave
  // the rest invisible to the scheduler no matter how idle they sat.
  bool provided_memory = memory.has_provider(MemoryType::Device);
  for (int ordinal = 0; ordinal < count; ++ordinal) {
    auto dev = std::make_unique<CudaComputeBackend>(ids.next_as<DeviceID>(), ordinal);
    if (!dev->on_gpu()) {
      rep.note += (rep.note.empty() ? "" : "; ") + ("device " + std::to_string(ordinal) + ": " + dev->unavailable());
      continue;
    }
    if (!provided_memory && !memory.has_provider(MemoryType::Device) && dev->gpu() && dev->gpu()->ctx) {
      memory.register_provider(std::make_unique<CudaDeviceMemoryProvider>(host, dev->gpu(), dev->capacity().memory));
      provided_memory = true;
    }
    rep.devices.push_back(dev->name());
    devices.add(std::move(dev));
  }
  return rep;
}

}  // namespace aios::compute
