// Apple Metal, bound at runtime through the Objective-C runtime so this file
// compiles as plain C++ on every host and only does anything on Darwin. The
// kernels are Metal Shading Language compiled by the driver at device start;
// every operation the device claims in supports() has one below, and
// execute() runs it — a device that returned Ok() without running anything is
// exactly the kind of status surface this codebase is not allowed to have.
#include "aios/compute/backends.hpp"
#include "aios/core/bytes.hpp"
#include "aios/hal/host.hpp"
#include <cstring>
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#define AIOS_METAL_RUNTIME 1
#elif defined(AIOS_METAL_SYNTAX_CHECK)
// A Linux compile of the Darwin path, so a change here is at least a valid
// program before it reaches a Mac. Nothing links; -fsyntax-only only.
extern "C" int sysctlbyname(const char*, void*, size_t*, void*, size_t);
#define AIOS_METAL_RUNTIME 1
#endif

namespace aios::compute {
namespace {

uint64_t fnv1a_hash(const std::string& str) {
  uint64_t hash = 14695981039346656037ull;
  for (char c : str) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

#if AIOS_METAL_RUNTIME

typedef void* oid;
typedef void* SEL;
typedef unsigned long NSUInteger;
struct MTLSize { NSUInteger width, height, depth; };

extern "C" {
  oid objc_getClass(const char* name);
  SEL sel_registerName(const char* name);
  void objc_msgSend(void);
  void* objc_autoreleasePoolPush(void);
  void objc_autoreleasePoolPop(void*);
}

// objc_msgSend must be called through a pointer of the method's exact
// signature (arm64 passes variadic arguments differently), so every call
// site names its return and argument types.
template <typename R, typename... A>
R send(oid self, const char* sel, A... a) {
  using F = R (*)(oid, SEL, A...);
  return reinterpret_cast<F>(objc_msgSend)(self, sel_registerName(sel), a...);
}
oid nsstring(const char* s) { return send<oid, const char*>((oid)objc_getClass("NSString"), "stringWithUTF8String:", s); }
std::string cstr(oid nsstr) {
  if (!nsstr) return {};
  const char* p = send<const char*>(nsstr, "UTF8String");
  return p ? p : "";
}
std::string describe_error(oid err) {
  if (!err) return "unknown error";
  return cstr(send<oid>(err, "localizedDescription"));
}
struct Pool {
  void* p;
  Pool() : p(objc_autoreleasePoolPush()) {}
  ~Pool() { objc_autoreleasePoolPop(p); }
};
void release(oid o) { if (o) send<void>(o, "release"); }

// Every kernel reads its sizes from this block at buffer 15 and bounds-checks
// its own thread oid, so a grid rounded up to whole threadgroups is safe.
struct Params { uint32_t M, K, N, H; float s; uint32_t n; };

const char* kMetalSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Params { uint M; uint K; uint N; uint H; float s; uint n; };

kernel void aios_fill(device float* out [[buffer(0)]], constant Params& p [[buffer(15)]], uint i [[thread_position_in_grid]]) { if (i < p.n) out[i] = p.s; }
kernel void aios_copy(device const float* a [[buffer(0)]], device float* out [[buffer(1)]], constant Params& p [[buffer(15)]], uint i [[thread_position_in_grid]]) { if (i < p.n) out[i] = a[i]; }
kernel void aios_scale(device const float* a [[buffer(0)]], device float* out [[buffer(1)]], constant Params& p [[buffer(15)]], uint i [[thread_position_in_grid]]) { if (i < p.n) out[i] = a[i] * p.s; }
kernel void aios_relu(device const float* a [[buffer(0)]], device float* out [[buffer(1)]], constant Params& p [[buffer(15)]], uint i [[thread_position_in_grid]]) { if (i < p.n) out[i] = a[i] > 0.0f ? a[i] : 0.0f; }
kernel void aios_gelu(device const float* a [[buffer(0)]], device float* out [[buffer(1)]], constant Params& p [[buffer(15)]], uint i [[thread_position_in_grid]]) {
  if (i >= p.n) return;
  float x = a[i];
  out[i] = 0.5f * x * (1.0f + tanh(0.7978845608f * (x + 0.044715f * x * x * x)));
}
kernel void aios_add(device const float* a [[buffer(0)]], device const float* b [[buffer(1)]], device float* out [[buffer(2)]], constant Params& p [[buffer(15)]], uint i [[thread_position_in_grid]]) { if (i < p.n) out[i] = a[i] + b[i]; }
kernel void aios_mul(device const float* a [[buffer(0)]], device const float* b [[buffer(1)]], device float* out [[buffer(2)]], constant Params& p [[buffer(15)]], uint i [[thread_position_in_grid]]) { if (i < p.n) out[i] = a[i] * b[i]; }
kernel void aios_relu_grad(device const float* dy [[buffer(0)]], device const float* x [[buffer(1)]], device float* dx [[buffer(2)]], constant Params& p [[buffer(15)]], uint i [[thread_position_in_grid]]) { if (i < p.n) dx[i] = x[i] > 0.0f ? dy[i] : 0.0f; }
kernel void aios_gelu_grad(device const float* dy [[buffer(0)]], device const float* xin [[buffer(1)]], device float* dx [[buffer(2)]], constant Params& p [[buffer(15)]], uint i [[thread_position_in_grid]]) {
  if (i >= p.n) return;
  float x = xin[i];
  float c = 0.7978845608f;
  float inner = c * (x + 0.044715f * x * x * x);
  float t = tanh(inner);
  float dinner = c * (1.0f + 3.0f * 0.044715f * x * x);
  dx[i] = dy[i] * (0.5f * (1.0f + t) + 0.5f * x * (1.0f - t * t) * dinner);
}
kernel void aios_reduce_sum(device const float* a [[buffer(0)]], device float* out [[buffer(1)]], constant Params& p [[buffer(15)]], uint i [[thread_position_in_grid]]) {
  if (i != 0) return;
  float acc = 0.0f;
  for (uint j = 0; j < p.n; ++j) acc += a[j];
  out[0] = acc;
}
kernel void aios_matmul(device const float* A [[buffer(0)]], device const float* B [[buffer(1)]], device float* C [[buffer(2)]], constant Params& p [[buffer(15)]], uint2 g [[thread_position_in_grid]]) {
  uint r = g.y, c = g.x;
  if (r >= p.M || c >= p.N) return;
  float acc = 0.0f;
  for (uint k = 0; k < p.K; ++k) acc += A[r * p.K + k] * B[k * p.N + c];
  C[r * p.N + c] = acc;
}
kernel void aios_matmul_grad_a(device const float* dC [[buffer(0)]], device const float* B [[buffer(1)]], device float* dA [[buffer(2)]], constant Params& p [[buffer(15)]], uint2 g [[thread_position_in_grid]]) {
  uint r = g.y, c = g.x;   // dA[MxK] = dC[MxN] · B[KxN]^T
  if (r >= p.M || c >= p.K) return;
  float acc = 0.0f;
  for (uint n = 0; n < p.N; ++n) acc += dC[r * p.N + n] * B[c * p.N + n];
  dA[r * p.K + c] = acc;
}
kernel void aios_matmul_grad_b(device const float* A [[buffer(0)]], device const float* dC [[buffer(1)]], device float* dB [[buffer(2)]], constant Params& p [[buffer(15)]], uint2 g [[thread_position_in_grid]]) {
  uint r = g.y, c = g.x;   // dB[KxN] = A[MxK]^T · dC[MxN]
  if (r >= p.K || c >= p.N) return;
  float acc = 0.0f;
  for (uint m = 0; m < p.M; ++m) acc += A[m * p.K + r] * dC[m * p.N + c];
  dB[r * p.N + c] = acc;
}
kernel void aios_bias_add(device const float* x [[buffer(0)]], device const float* bias [[buffer(1)]], device float* out [[buffer(2)]], constant Params& p [[buffer(15)]], uint2 g [[thread_position_in_grid]]) {
  uint r = g.y, c = g.x;
  if (r >= p.M || c >= p.N) return;
  out[r * p.N + c] = x[r * p.N + c] + bias[c];
}
kernel void aios_bias_add_grad(device const float* dy [[buffer(0)]], device float* db [[buffer(1)]], constant Params& p [[buffer(15)]], uint c [[thread_position_in_grid]]) {
  if (c >= p.N) return;
  float s = 0.0f;
  for (uint m = 0; m < p.M; ++m) s += dy[m * p.N + c];
  db[c] = s;
}
kernel void aios_layernorm(device const float* x [[buffer(0)]], device const float* gamma [[buffer(1)]], device const float* beta [[buffer(2)]], device float* out [[buffer(3)]], device float* mu_out [[buffer(4)]], device float* inv_out [[buffer(5)]], constant Params& p [[buffer(15)]], uint r [[thread_position_in_grid]]) {
  if (r >= p.M) return;
  float mu = 0.0f;
  for (uint c = 0; c < p.N; ++c) mu += x[r * p.N + c];
  mu /= (float)p.N;
  float var = 0.0f;
  for (uint c = 0; c < p.N; ++c) { float d = x[r * p.N + c] - mu; var += d * d; }
  var /= (float)p.N;
  float inv = rsqrt(var + 1e-5f);
  mu_out[r] = mu; inv_out[r] = inv;
  for (uint c = 0; c < p.N; ++c) out[r * p.N + c] = gamma[c] * (x[r * p.N + c] - mu) * inv + beta[c];
}
kernel void aios_layernorm_grad_x(device const float* dy [[buffer(0)]], device const float* x [[buffer(1)]], device const float* gamma [[buffer(2)]], device float* dx [[buffer(3)]], constant Params& p [[buffer(15)]], uint r [[thread_position_in_grid]]) {
  if (r >= p.M) return;
  float mu = 0.0f;
  for (uint c = 0; c < p.N; ++c) mu += x[r * p.N + c];
  mu /= (float)p.N;
  float var = 0.0f;
  for (uint c = 0; c < p.N; ++c) { float d = x[r * p.N + c] - mu; var += d * d; }
  var /= (float)p.N;
  float inv = rsqrt(var + 1e-5f);
  float sum_d = 0.0f, sum_dn = 0.0f;
  for (uint c = 0; c < p.N; ++c) {
    float xh = (x[r * p.N + c] - mu) * inv;
    float dn = dy[r * p.N + c] * gamma[c];
    sum_d += dn; sum_dn += dn * xh;
  }
  for (uint c = 0; c < p.N; ++c) {
    float xh = (x[r * p.N + c] - mu) * inv;
    float dn = dy[r * p.N + c] * gamma[c];
    dx[r * p.N + c] = inv * (dn - (sum_d + xh * sum_dn) / (float)p.N);
  }
}
kernel void aios_layernorm_grad_gb(device const float* dy [[buffer(0)]], device const float* x [[buffer(1)]], device float* dgamma [[buffer(2)]], device float* dbeta [[buffer(3)]], constant Params& p [[buffer(15)]], uint c [[thread_position_in_grid]]) {
  if (c >= p.N) return;
  float dg = 0.0f, db = 0.0f;
  for (uint r = 0; r < p.M; ++r) {
    float mu = 0.0f;
    for (uint k = 0; k < p.N; ++k) mu += x[r * p.N + k];
    mu /= (float)p.N;
    float var = 0.0f;
    for (uint k = 0; k < p.N; ++k) { float d = x[r * p.N + k] - mu; var += d * d; }
    var /= (float)p.N;
    float inv = rsqrt(var + 1e-5f);
    dg += dy[r * p.N + c] * (x[r * p.N + c] - mu) * inv;
    db += dy[r * p.N + c];
  }
  dgamma[c] = dg; dbeta[c] = db;
}
kernel void aios_softmax_ce_probs(device const float* logits [[buffer(0)]], device float* probs [[buffer(1)]], constant Params& p [[buffer(15)]], uint t [[thread_position_in_grid]]) {
  if (t >= p.M) return;   // M rows (targets), N = vocabulary
  float best = -1e30f;
  for (uint w = 0; w < p.N; ++w) { float v = logits[t * p.N + w]; probs[t * p.N + w] = v; if (v > best) best = v; }
  float sum = 0.0f;
  for (uint w = 0; w < p.N; ++w) { float e = exp(probs[t * p.N + w] - best); probs[t * p.N + w] = e; sum += e; }
  float inv = sum > 0.0f ? 1.0f / sum : 0.0f;
  for (uint w = 0; w < p.N; ++w) probs[t * p.N + w] *= inv;
}
kernel void aios_softmax_ce_loss(device const float* probs [[buffer(0)]], device const float* targets [[buffer(1)]], device float* loss [[buffer(2)]], constant Params& p [[buffer(15)]], uint i [[thread_position_in_grid]]) {
  if (i != 0) return;
  float total = 0.0f; uint cnt = 0;
  for (uint t = 0; t < p.M; ++t) {
    int tgt = (int)targets[t];
    if (tgt >= 0 && tgt < (int)p.N) { float q = probs[t * p.N + tgt]; total += -log(q > 1e-12f ? q : 1e-12f); ++cnt; }
  }
  loss[0] = cnt ? total / (float)cnt : 0.0f;
}
kernel void aios_softmax_ce_grad(device const float* probs [[buffer(0)]], device const float* targets [[buffer(1)]], device float* dl [[buffer(2)]], constant Params& p [[buffer(15)]], uint2 g [[thread_position_in_grid]]) {
  uint t = g.y, w = g.x;
  if (t >= p.M || w >= p.N) return;
  uint cnt = 0;
  for (uint r = 0; r < p.M; ++r) { int tg = (int)targets[r]; if (tg >= 0 && tg < (int)p.N) ++cnt; }
  float norm = cnt ? 1.0f / (float)cnt : 0.0f;
  int tgt = (int)targets[t];
  bool valid = tgt >= 0 && tgt < (int)p.N;
  float d = valid ? probs[t * p.N + w] : 0.0f;
  if (valid && (int)w == tgt) d -= 1.0f;
  dl[t * p.N + w] = d * norm;
}
kernel void aios_adam_step(device float* w [[buffer(0)]], device const float* g [[buffer(1)]], device float* m [[buffer(2)]], device float* v [[buffer(3)]], constant Params& p [[buffer(15)]], uint i [[thread_position_in_grid]]) {
  if (i >= p.n) return;
  const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f, decay = 0.01f;
  float grad = g[i];
  float mi = b1 * m[i] + (1.0f - b1) * grad;
  float vi = b2 * v[i] + (1.0f - b2) * grad * grad;
  m[i] = mi; v[i] = vi;
  w[i] -= p.s * (mi / (sqrt(vi) + eps) + decay * w[i]);
}
kernel void aios_causal_attention(device const float* q [[buffer(0)]], device const float* k [[buffer(1)]], device const float* v [[buffer(2)]], device float* o [[buffer(3)]], device float* att [[buffer(4)]], constant Params& p [[buffer(15)]], uint idx [[thread_position_in_grid]]) {
  uint T = p.M, D = p.K, H = p.H;
  if (idx >= H * T) return;
  uint h = idx / T, t = idx % T, dh = D / H;
  float scale = rsqrt((float)dh);
  device float* row = att + (h * T + t) * T;
  float best = -1e30f;
  for (uint s2 = 0; s2 <= t; ++s2) {
    float dot = 0.0f;
    for (uint j = 0; j < dh; ++j) dot += q[t * D + h * dh + j] * k[s2 * D + h * dh + j];
    row[s2] = dot * scale;
    if (row[s2] > best) best = row[s2];
  }
  for (uint s2 = t + 1; s2 < T; ++s2) row[s2] = 0.0f;
  float sum = 0.0f;
  for (uint s2 = 0; s2 <= t; ++s2) { row[s2] = exp(row[s2] - best); sum += row[s2]; }
  float inv = sum > 0.0f ? 1.0f / sum : 0.0f;
  for (uint s2 = 0; s2 <= t; ++s2) row[s2] *= inv;
  for (uint j = 0; j < dh; ++j) {
    float acc = 0.0f;
    for (uint s2 = 0; s2 <= t; ++s2) acc += row[s2] * v[s2 * D + h * dh + j];
    o[t * D + h * dh + j] = acc;
  }
}
kernel void aios_causal_attention_grad_q(device const float* dO [[buffer(0)]], device const float* q [[buffer(1)]], device const float* k [[buffer(2)]], device const float* v [[buffer(3)]], device const float* att [[buffer(4)]], device float* ds [[buffer(5)]], device float* dq [[buffer(6)]], constant Params& p [[buffer(15)]], uint idx [[thread_position_in_grid]]) {
  uint T = p.M, D = p.K, H = p.H;
  if (idx >= H * T) return;
  uint h = idx / T, t = idx % T, dh = D / H;
  float scale = rsqrt((float)dh);
  device const float* arow = att + (h * T + t) * T;
  device float* drow = ds + (h * T + t) * T;
  float dot = 0.0f;
  for (uint s2 = 0; s2 <= t; ++s2) {
    float dA = 0.0f;
    for (uint j = 0; j < dh; ++j) dA += dO[t * D + h * dh + j] * v[s2 * D + h * dh + j];
    drow[s2] = dA; dot += arow[s2] * dA;
  }
  for (uint s2 = t + 1; s2 < T; ++s2) drow[s2] = 0.0f;
  for (uint j = 0; j < dh; ++j) dq[t * D + h * dh + j] = 0.0f;
  for (uint s2 = 0; s2 <= t; ++s2) {
    float d = arow[s2] * (drow[s2] - dot) * scale;
    drow[s2] = d;
    for (uint j = 0; j < dh; ++j) dq[t * D + h * dh + j] += d * k[s2 * D + h * dh + j];
  }
}
kernel void aios_causal_attention_grad_kv(device const float* dO [[buffer(0)]], device const float* q [[buffer(1)]], device const float* att [[buffer(2)]], device const float* ds [[buffer(3)]], device float* dk [[buffer(4)]], device float* dv [[buffer(5)]], constant Params& p [[buffer(15)]], uint idx [[thread_position_in_grid]]) {
  uint T = p.M, D = p.K, H = p.H;
  if (idx >= H * T) return;
  uint h = idx / T, s2 = idx % T, dh = D / H;
  for (uint j = 0; j < dh; ++j) {
    float ak = 0.0f, av = 0.0f;
    for (uint t = s2; t < T; ++t) {
      ak += ds[(h * T + t) * T + s2] * q[t * D + h * dh + j];
      av += att[(h * T + t) * T + s2] * dO[t * D + h * dh + j];
    }
    dk[s2 * D + h * dh + j] = ak;
    dv[s2 * D + h * dh + j] = av;
  }
}
)MSL";

const char* const kKernelNames[] = {
  "aios_fill", "aios_copy", "aios_scale", "aios_relu", "aios_gelu", "aios_add", "aios_mul", "aios_relu_grad", "aios_gelu_grad",
  "aios_reduce_sum", "aios_matmul", "aios_matmul_grad_a", "aios_matmul_grad_b", "aios_bias_add", "aios_bias_add_grad",
  "aios_layernorm", "aios_layernorm_grad_x", "aios_layernorm_grad_gb", "aios_softmax_ce_probs", "aios_softmax_ce_loss",
  "aios_softmax_ce_grad", "aios_adam_step", "aios_causal_attention", "aios_causal_attention_grad_q", "aios_causal_attention_grad_kv",
};

uint64_t host_memory_bytes() {
  uint64_t v = 0; size_t n = sizeof v;
  if (sysctlbyname("hw.memsize", &v, &n, nullptr, 0) != 0) return 0;
  return v;
}

#endif  // AIOS_METAL_RUNTIME

}  // namespace

#if AIOS_METAL_RUNTIME

struct MetalComputeBackend::Gpu {
  oid device = nullptr;
  oid queue = nullptr;
  oid library = nullptr;
  std::unordered_map<std::string, oid> pipelines;
  NSUInteger max_threads = 256;
  NSUInteger simd_width = 32;
  ~Gpu() {
    for (auto& [n, p] : pipelines) release(p);
    release(library); release(queue); release(device);
  }
};

MetalComputeBackend::MetalComputeBackend(const DeviceID& id, int ordinal) : id_(id) {
  Pool pool;
  void* metal_lib = hal::load_library("/System/Library/Frameworks/Metal.framework/Metal");
  if (!metal_lib) { unavailable_ = "Metal framework not found"; return; }

  typedef oid (*MTLCopyAllDevicesFn)(void);
  typedef oid (*MTLCreateSystemDefaultDeviceFn)(void);
  auto copy_all = reinterpret_cast<MTLCopyAllDevicesFn>(hal::library_symbol(metal_lib, "MTLCopyAllDevices"));
  auto create_def = reinterpret_cast<MTLCreateSystemDefaultDeviceFn>(hal::library_symbol(metal_lib, "MTLCreateSystemDefaultDevice"));

  oid target = nullptr;
  if (copy_all) {
    oid arr = copy_all();
    if (arr) {
      NSUInteger cnt = send<NSUInteger>(arr, "count");
      if (ordinal >= 0 && (NSUInteger)ordinal < cnt) {
        target = send<oid, NSUInteger>(arr, "objectAtIndex:", (NSUInteger)ordinal);
        send<void>(target, "retain");
      }
      release(arr);
    }
  }
  if (!target && create_def && ordinal == 0) target = create_def();
  if (!target) { unavailable_ = "Metal device ordinal " + std::to_string(ordinal) + " not found"; return; }

  auto gpu = std::make_unique<Gpu>();
  gpu->device = target;
  std::string dev_name = cstr(send<oid>(gpu->device, "name"));
  if (dev_name.empty()) dev_name = "Apple GPU";

  // Compile the kernels for THIS device. A device whose driver refuses the
  // source is reported unavailable with the compiler's own words, not
  // registered with a pipeline map that is empty.
  oid err = nullptr;
  gpu->library = send<oid, oid, oid, oid*>(gpu->device, "newLibraryWithSource:options:error:", nsstring(kMetalSource), nullptr, &err);
  if (!gpu->library) { unavailable_ = "Metal shader compile failed: " + describe_error(err); return; }
  for (const char* name : kKernelNames) {
    oid fn = send<oid, oid>(gpu->library, "newFunctionWithName:", nsstring(name));
    if (!fn) { unavailable_ = std::string("the compiled Metal library has no ") + name; return; }
    oid pso = send<oid, oid, oid*>(gpu->device, "newComputePipelineStateWithFunction:error:", fn, &err);
    release(fn);
    if (!pso) { unavailable_ = std::string("pipeline for ") + name + ": " + describe_error(err); return; }
    gpu->pipelines[name] = pso;
    NSUInteger mt = send<NSUInteger>(pso, "maxTotalThreadsPerThreadgroup");
    if (mt && mt < gpu->max_threads) gpu->max_threads = mt;
    NSUInteger w = send<NSUInteger>(pso, "threadExecutionWidth");
    if (w) gpu->simd_width = w;
  }
  gpu->queue = send<oid>(gpu->device, "newCommandQueue");
  if (!gpu->queue) { unavailable_ = "Metal device gave no command queue"; return; }

  // What the device reports about itself. recommendedMaxWorkingSetSize is the
  // driver's own figure for how much this GPU may hold; on unified memory that
  // is a share of system RAM, and the host's total is the ceiling when the
  // driver declines to say. No core count is public, so no FLOP rate is
  // claimed (the scheduler weighs the device by what it can be told, as for
  // Vulkan); the lane count is the SIMD width the pipelines actually execute at.
  uint64_t max_mem = send<uint64_t>(gpu->device, "recommendedMaxWorkingSetSize");
  if (max_mem == 0) max_mem = host_memory_bytes();
  bool unified = send<bool>(gpu->device, "hasUnifiedMemory");
  cap_.lanes = (uint32_t)gpu->simd_width;
  cap_.memory = max_mem;
  cap_.native_memory = MemoryType::Device;
  cap_.reads_host_memory = true;   // copies from the operands' host windows itself
  cap_.flops_per_second = 0;
  name_ = "metal:" + dev_name + (unified ? " unified" : "") + " [" + std::to_string(ordinal) + "]";
  gpu_ = std::move(gpu);
}

MetalComputeBackend::~MetalComputeBackend() = default;

bool MetalComputeBackend::supports(const Operation& op) const {
  if (!gpu_) return false;
  if (op.code == OpCode::Custom) { hal::LockGuard g(mu_); return kernels_.count(op.custom_kernel) > 0; }
  switch (op.code) {
    case OpCode::Fill: case OpCode::Add: case OpCode::Mul: case OpCode::Scale:
    case OpCode::Relu: case OpCode::Copy: case OpCode::MatMul: case OpCode::ReduceSum:
    case OpCode::Gelu: case OpCode::GeluGrad: case OpCode::ReluGrad:
    case OpCode::MatMulGradA: case OpCode::MatMulGradB:
    case OpCode::BiasAdd: case OpCode::BiasAddGrad:
    case OpCode::LayerNorm: case OpCode::LayerNormGrad:
    case OpCode::SoftmaxCrossEntropyLoss: case OpCode::SoftmaxCrossEntropyGrad:
    case OpCode::AdamStep: case OpCode::CausalAttention: case OpCode::CausalAttentionGrad:
      return op.inputs.empty() || op.inputs[0].dtype == DType::F32;
    default:
      return false;
  }
}

Result<void> MetalComputeBackend::register_kernel(const std::string& name, CustomKernel k) {
  hal::LockGuard g(mu_);
  kernels_[fnv1a_hash(name)] = std::move(k);
  return Ok();
}

uint32_t MetalComputeBackend::busy_lanes() const { return busy_lanes_.load(); }

Result<DeviceMemoryStats> MetalComputeBackend::memory_stats() const {
  if (!gpu_) return Result<DeviceMemoryStats>(Status::Unsupported, "no Metal device: " + unavailable_);
  DeviceMemoryStats s;
  s.total_bytes = cap_.memory;
  // Metal exposes this PROCESS's allocations on the device, not the card's
  // total; the source says so rather than letting the number pass for one.
  s.used_bytes = send<NSUInteger>(gpu_->device, "currentAllocatedSize");
  s.free_bytes = s.total_bytes > s.used_bytes ? s.total_bytes - s.used_bytes : 0;
  s.source = "metal:currentAllocatedSize(this process)";
  return s;
}

Result<void> MetalComputeBackend::execute(const Operation& op, MemoryFabric& fabric, uint32_t lanes) {
  if (!gpu_) return Result<void>(Status::Unsupported, "no Metal device: " + unavailable_);
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
    { hal::LockGuard lg(mu_); auto it = kernels_.find(op.custom_kernel); if (it == kernels_.end()) return Result<void>(Status::NotFound, "custom kernel not registered on this device"); k = it->second; }
    KernelArgs args{op, in, out, lanes};
    auto r = k(args);
    if (r.ok()) for (const auto& t : op.outputs) fabric.touch(t.memory);
    return r;
  }

  Pool pool;
  auto host_in = [&](size_t i) { return in[i].base + op.inputs[i].offset; };
  auto host_out = [&](size_t i) { return out[i].base + op.outputs[i].offset; };

  // Shared-storage buffers: the CPU writes them, the GPU reads them, and the
  // outputs are copied back into fabric memory when the command buffer is done.
  std::vector<oid> owned;
  auto cleanup = [&]() { for (oid b : owned) release(b); owned.clear(); };
  auto buf_in = [&](size_t i) -> oid {
    oid b = send<oid, const void*, NSUInteger, NSUInteger>(gpu_->device, "newBufferWithBytes:length:options:", host_in(i), (NSUInteger)std::max<uint64_t>(op.inputs[i].bytes(), 4), (NSUInteger)0);
    owned.push_back(b); return b;
  };
  auto buf_bytes = [&](uint64_t bytes) -> oid {
    oid b = send<oid, NSUInteger, NSUInteger>(gpu_->device, "newBufferWithLength:options:", (NSUInteger)std::max<uint64_t>(bytes, 4), (NSUInteger)0);
    owned.push_back(b); return b;
  };
  struct Out { oid buf; uint8_t* host; uint64_t bytes; };
  std::vector<Out> outs;
  std::vector<oid> inbufs;
  for (size_t i = 0; i < op.inputs.size(); ++i) inbufs.push_back(buf_in(i));
  auto buf_out = [&](size_t i) -> oid {
    // An output that IS an input (adam-step updates its weight in place) must
    // share the buffer, or the kernel would read the stale copy.
    for (size_t k = 0; k < op.inputs.size(); ++k) {
      if (op.inputs[k].memory == op.outputs[i].memory && op.inputs[k].offset == op.outputs[i].offset) { outs.push_back({inbufs[k], host_out(i), op.outputs[i].bytes()}); return inbufs[k]; }
    }
    oid b = buf_bytes(op.outputs[i].bytes());
    outs.push_back({b, host_out(i), op.outputs[i].bytes()});
    return b;
  };
  for (oid b : inbufs) if (!b) { cleanup(); return Result<void>(Status::Exhausted, "Metal refused a buffer allocation"); }

  Params p{};
  std::string kernel;
  std::vector<oid> bind;   // buffers 0..k in kernel order
  uint64_t gx = 1, gy = 1;
  bool grid2d = false;
  auto shape2 = [&](const Tensor& t, uint64_t& rows, uint64_t& cols) { if (t.rank() < 2) return false; rows = t.shape[t.rank() - 2]; cols = t.shape[t.rank() - 1]; return true; };
  const uint64_t n_out = op.outputs.empty() ? 0 : op.outputs[0].elements();
  // A second pass (some operations are two kernels).
  std::string kernel2; std::vector<oid> bind2; uint64_t gx2 = 1, gy2 = 1; bool grid2d2 = false; Params p2{};

  switch (op.code) {
    case OpCode::Fill: { if (out.empty()) return Result<void>(Status::Invalid, "fill needs an output"); kernel = "aios_fill"; bind = {buf_out(0)}; p.s = (float)op.scalar; p.n = (uint32_t)n_out; gx = n_out; break; }
    case OpCode::Copy: case OpCode::Relu: case OpCode::Gelu: case OpCode::Scale: {
      if (in.empty() || out.empty()) return Result<void>(Status::Invalid, "needs one input and one output");
      kernel = op.code == OpCode::Copy ? "aios_copy" : op.code == OpCode::Relu ? "aios_relu" : op.code == OpCode::Gelu ? "aios_gelu" : "aios_scale";
      uint64_t n = std::min(n_out, op.inputs[0].elements());
      bind = {inbufs[0], buf_out(0)}; p.s = (float)op.scalar; p.n = (uint32_t)n; gx = n; break;
    }
    case OpCode::Add: case OpCode::Mul: case OpCode::GeluGrad: case OpCode::ReluGrad: {
      if (in.size() < 2 || out.empty()) return Result<void>(Status::Invalid, "needs two inputs and an output");
      kernel = op.code == OpCode::Add ? "aios_add" : op.code == OpCode::Mul ? "aios_mul" : op.code == OpCode::GeluGrad ? "aios_gelu_grad" : "aios_relu_grad";
      uint64_t n = std::min({n_out, op.inputs[0].elements(), op.inputs[1].elements()});
      bind = {inbufs[0], inbufs[1], buf_out(0)}; p.n = (uint32_t)n; gx = n; break;
    }
    case OpCode::ReduceSum: { if (in.empty() || out.empty()) return Result<void>(Status::Invalid, "reduce-sum needs in, out"); kernel = "aios_reduce_sum"; bind = {inbufs[0], buf_out(0)}; p.n = (uint32_t)op.inputs[0].elements(); gx = 1; break; }
    case OpCode::MatMul: case OpCode::MatMulGradA: case OpCode::MatMulGradB: {
      uint64_t r0, c0, r1, c1;
      if (in.size() < 2 || out.empty() || !shape2(op.inputs[0], r0, c0) || !shape2(op.inputs[1], r1, c1)) return Result<void>(Status::Invalid, "matmul needs two matrices");
      grid2d = true; bind = {inbufs[0], inbufs[1], buf_out(0)};
      if (op.code == OpCode::MatMul) { p.M = (uint32_t)r0; p.K = (uint32_t)c0; p.N = (uint32_t)c1; if (r1 != c0) return Result<void>(Status::Invalid, "matmul inner dimensions disagree"); kernel = "aios_matmul"; gx = p.N; gy = p.M; }
      else if (op.code == OpCode::MatMulGradA) { p.M = (uint32_t)r0; p.N = (uint32_t)c0; p.K = (uint32_t)r1; if (c1 != c0) return Result<void>(Status::Invalid, "matmul-grad-a shape mismatch"); kernel = "aios_matmul_grad_a"; gx = p.K; gy = p.M; }
      else { p.M = (uint32_t)r0; p.K = (uint32_t)c0; p.N = (uint32_t)c1; if (r1 != r0) return Result<void>(Status::Invalid, "matmul-grad-b shape mismatch"); kernel = "aios_matmul_grad_b"; gx = p.N; gy = p.K; }
      break;
    }
    case OpCode::BiasAdd: {
      if (in.size() < 2 || out.empty()) return Result<void>(Status::Invalid, "bias-add needs x, bias");
      p.N = (uint32_t)op.inputs[1].elements(); if (!p.N) return Result<void>(Status::Invalid, "empty bias"); p.M = (uint32_t)(op.inputs[0].elements() / p.N);
      kernel = "aios_bias_add"; grid2d = true; bind = {inbufs[0], inbufs[1], buf_out(0)}; gx = p.N; gy = p.M; break;
    }
    case OpCode::BiasAddGrad: {
      if (in.empty() || out.empty()) return Result<void>(Status::Invalid, "bias-add-grad needs dy");
      p.N = (uint32_t)n_out; if (!p.N) return Result<void>(Status::Invalid, "empty bias"); p.M = (uint32_t)(op.inputs[0].elements() / p.N);
      kernel = "aios_bias_add_grad"; bind = {inbufs[0], buf_out(0)}; gx = p.N; break;
    }
    case OpCode::LayerNorm: {
      if (in.size() < 3 || out.empty()) return Result<void>(Status::Invalid, "layernorm needs x, gamma, beta");
      p.N = (uint32_t)op.inputs[1].elements(); if (!p.N) return Result<void>(Status::Invalid, "empty gamma"); p.M = (uint32_t)(op.inputs[0].elements() / p.N);
      oid y = buf_out(0);
      oid mu = out.size() > 1 ? buf_out(1) : buf_bytes((uint64_t)p.M * 4);
      oid inv = out.size() > 2 ? buf_out(2) : buf_bytes((uint64_t)p.M * 4);
      kernel = "aios_layernorm"; bind = {inbufs[0], inbufs[1], inbufs[2], y, mu, inv}; gx = p.M; break;
    }
    case OpCode::LayerNormGrad: {
      if (in.size() < 3 || out.size() < 3) return Result<void>(Status::Invalid, "layernorm-grad needs dy, x, gamma and dx, dgamma, dbeta");
      p.N = (uint32_t)op.inputs[2].elements(); if (!p.N) return Result<void>(Status::Invalid, "empty gamma"); p.M = (uint32_t)(op.inputs[1].elements() / p.N);
      oid dx = buf_out(0), dg = buf_out(1), db = buf_out(2);
      kernel = "aios_layernorm_grad_x"; bind = {inbufs[0], inbufs[1], inbufs[2], dx}; gx = p.M;
      kernel2 = "aios_layernorm_grad_gb"; bind2 = {inbufs[0], inbufs[1], dg, db}; gx2 = p.N; p2 = p; break;
    }
    case OpCode::SoftmaxCrossEntropyLoss: {
      if (in.size() < 2 || out.size() < 2) return Result<void>(Status::Invalid, "softmax-ce-loss needs logits, targets and loss, probs");
      p.M = (uint32_t)op.inputs[1].elements(); if (!p.M) return Result<void>(Status::Invalid, "empty targets"); p.N = (uint32_t)(op.inputs[0].elements() / p.M);
      oid loss = buf_out(0), probs = buf_out(1);
      kernel = "aios_softmax_ce_probs"; bind = {inbufs[0], probs}; gx = p.M;
      kernel2 = "aios_softmax_ce_loss"; bind2 = {probs, inbufs[1], loss}; gx2 = 1; p2 = p; break;
    }
    case OpCode::SoftmaxCrossEntropyGrad: {
      if (in.size() < 2 || out.empty()) return Result<void>(Status::Invalid, "softmax-ce-grad needs probs, targets");
      p.M = (uint32_t)op.inputs[1].elements(); if (!p.M) return Result<void>(Status::Invalid, "empty targets"); p.N = (uint32_t)(op.inputs[0].elements() / p.M);
      kernel = "aios_softmax_ce_grad"; grid2d = true; bind = {inbufs[0], inbufs[1], buf_out(0)}; gx = p.N; gy = p.M; break;
    }
    case OpCode::AdamStep: {
      if (in.size() < 4 || out.empty()) return Result<void>(Status::Invalid, "adam-step needs w, grad, m, v");
      p.n = (uint32_t)n_out; p.s = op.scalar > 0.0 ? (float)op.scalar : 0.0f;   // as given (0 moves nothing)
      oid w = buf_out(0);
      // m and v are updated in place too: read them back into their inputs.
      outs.push_back({inbufs[2], in[2].base + op.inputs[2].offset, op.inputs[2].bytes()});
      outs.push_back({inbufs[3], in[3].base + op.inputs[3].offset, op.inputs[3].bytes()});
      kernel = "aios_adam_step"; bind = {w, inbufs[1], inbufs[2], inbufs[3]}; gx = p.n; break;
    }
    case OpCode::CausalAttention: {
      uint64_t T, D;
      if (in.size() < 3 || out.size() < 2 || !shape2(op.inputs[0], T, D)) return Result<void>(Status::Invalid, "causal-attention needs q,k,v [TxD] and o, att");
      p.M = (uint32_t)T; p.K = (uint32_t)D; p.H = op.scalar >= 1.0 ? (uint32_t)op.scalar : 1;
      if (D % p.H || op.outputs[1].elements() != (uint64_t)p.H * T * T) return Result<void>(Status::Invalid, "causal-attention: heads must divide D and att must be [HxTxT]");
      kernel = "aios_causal_attention"; bind = {inbufs[0], inbufs[1], inbufs[2], buf_out(0), buf_out(1)}; gx = (uint64_t)p.H * T; break;
    }
    case OpCode::CausalAttentionGrad: {
      uint64_t T, D;
      if (in.size() < 5 || out.size() < 3 || !shape2(op.inputs[1], T, D)) return Result<void>(Status::Invalid, "causal-attention-grad needs dO,q,k,v,att and dq,dk,dv");
      p.M = (uint32_t)T; p.K = (uint32_t)D; p.H = op.scalar >= 1.0 ? (uint32_t)op.scalar : 1;
      if (D % p.H || op.inputs[4].elements() != (uint64_t)p.H * T * T) return Result<void>(Status::Invalid, "causal-attention-grad: heads must divide D and att must be [HxTxT]");
      oid ds = buf_bytes((uint64_t)p.H * T * T * 4);
      oid dq = buf_out(0), dk = buf_out(1), dv = buf_out(2);
      kernel = "aios_causal_attention_grad_q"; bind = {inbufs[0], inbufs[1], inbufs[2], inbufs[3], inbufs[4], ds, dq}; gx = (uint64_t)p.H * T;
      kernel2 = "aios_causal_attention_grad_kv"; bind2 = {inbufs[0], inbufs[1], inbufs[4], ds, dk, dv}; gx2 = gx; p2 = p; break;
    }
    default:
      cleanup();
      return Result<void>(Status::Unsupported, std::string("no Metal kernel for ") + opcode_name(op.code) + " on this device");
  }
  for (auto& o : outs) if (!o.buf) { cleanup(); return Result<void>(Status::Exhausted, "Metal refused a buffer allocation"); }

  auto encode = [&](oid enc, const std::string& name, const std::vector<oid>& bufs, const Params& prm, uint64_t x, uint64_t y, bool two_d) -> Result<void> {
    auto it = gpu_->pipelines.find(name);
    if (it == gpu_->pipelines.end()) return Result<void>(Status::NotFound, "no pipeline for " + name);
    send<void, oid>(enc, "setComputePipelineState:", it->second);
    for (size_t i = 0; i < bufs.size(); ++i) send<void, oid, NSUInteger, NSUInteger>(enc, "setBuffer:offset:atIndex:", bufs[i], (NSUInteger)0, (NSUInteger)i);
    send<void, const void*, NSUInteger, NSUInteger>(enc, "setBytes:length:atIndex:", &prm, (NSUInteger)sizeof prm, (NSUInteger)15);
    MTLSize tg, grid;
    if (two_d) { NSUInteger side = 16; while (side * side > gpu_->max_threads && side > 1) side /= 2; tg = {side, side, 1}; grid = {(x + side - 1) / side, (y + side - 1) / side, 1}; }
    else { NSUInteger w = std::min<NSUInteger>(gpu_->max_threads, 256); tg = {w, 1, 1}; grid = {(std::max<uint64_t>(x, 1) + w - 1) / w, 1, 1}; }
    send<void, MTLSize, MTLSize>(enc, "dispatchThreadgroups:threadsPerThreadgroup:", grid, tg);
    return Ok();
  };

  oid cmd = send<oid>(gpu_->queue, "commandBuffer");
  if (!cmd) { cleanup(); return Result<void>(Status::IoError, "Metal gave no command buffer"); }
  oid enc = send<oid>(cmd, "computeCommandEncoder");
  if (!enc) { cleanup(); return Result<void>(Status::IoError, "Metal gave no compute encoder"); }
  auto e = encode(enc, kernel, bind, p, gx, gy, grid2d);
  if (e.ok() && !kernel2.empty()) {
    // The second kernel reads what the first wrote: end the encoder so the
    // writes are visible, then encode again on the same command buffer.
    send<void>(enc, "endEncoding");
    enc = send<oid>(cmd, "computeCommandEncoder");
    if (!enc) { cleanup(); return Result<void>(Status::IoError, "Metal gave no compute encoder"); }
    e = encode(enc, kernel2, bind2, p2, gx2, gy2, grid2d2);
  }
  send<void>(enc, "endEncoding");
  if (!e.ok()) { cleanup(); return e; }
  send<void>(cmd, "commit");
  send<void>(cmd, "waitUntilCompleted");
  NSUInteger status = send<NSUInteger>(cmd, "status");
  if (status == 5 /* MTLCommandBufferStatusError */) {
    std::string why = describe_error(send<oid>(cmd, "error"));
    cleanup();
    return Result<void>(Status::IoError, "the Metal kernel faulted: " + why);
  }
  for (auto& o : outs) {
    void* contents = send<void*>(o.buf, "contents");
    if (contents) std::memcpy(o.host, contents, o.bytes);
  }
  cleanup();
  for (const auto& t : op.outputs) fabric.touch(t.memory);
  return Ok();
}

// Its address tags a Mapping whose device_ctx is a MetalDeviceBuffer.
char kMetalOwner;
struct MetalDeviceBuffer {
  oid buffer = nullptr;
  uint64_t size = 0;
};

class MetalDeviceMemoryProvider final : public MemoryProvider {
 public:
  MetalDeviceMemoryProvider(hal::Host& host, oid device, uint64_t capacity)
      : host_(host), device_(device), capacity_(capacity) {}
  MemoryType type() const override { return MemoryType::Device; }
  uint64_t capacity() const override { return capacity_; }

  Result<hal::Mapping> allocate(uint64_t size, const MemoryID&) override {
    auto m = host_.map(size, hal::MappingKind::Private);
    if (!m.ok()) return m;
    Pool pool;
    oid buf = send<oid, NSUInteger, NSUInteger>(device_, "newBufferWithLength:options:",
                                                 (NSUInteger)std::max<uint64_t>(size, 4), (NSUInteger)0);
    if (!buf) {
      host_.unmap(*m);
      return Result<hal::Mapping>(Status::Exhausted, "device memory allocation failed (Metal refused buffer)");
    }
    m->device_ctx = new MetalDeviceBuffer{buf, size};
    m->device_owner = &kMetalOwner;
    return m;
  }

  void release(hal::Mapping& m) override {
    if (m.device_ctx && m.device_owner == &kMetalOwner) {
      Pool pool;
      auto* b = static_cast<MetalDeviceBuffer*>(m.device_ctx);
      if (b->buffer) aios::compute::release(b->buffer);   // the class's own release(Mapping&) hides the oid helper
      delete b;
      m.device_ctx = nullptr;
      m.device_owner = nullptr;
    }
    host_.unmap(m);
  }

 private:
  hal::Host& host_;
  oid device_;
  uint64_t capacity_;
};

AcceleratorReport register_metal_devices(DeviceFabric& devices, MemoryFabric& memory, hal::Host& host, IdGenerator& ids) {
  AcceleratorReport rep;
  Pool pool;
  void* metal_lib = hal::load_library("/System/Library/Frameworks/Metal.framework/Metal");
  if (!metal_lib) { rep.note = "Metal framework not found"; return rep; }
  typedef oid (*MTLCopyAllDevicesFn)(void);
  auto copy_all = reinterpret_cast<MTLCopyAllDevicesFn>(hal::library_symbol(metal_lib, "MTLCopyAllDevices"));
  int count = 0;
  if (copy_all) {
    oid arr = copy_all();
    if (arr) { count = (int)send<NSUInteger>(arr, "count"); release(arr); }
  }
  if (count <= 0) count = 1;   // MTLCreateSystemDefaultDevice may still answer
  bool provided_memory = memory.has_provider(MemoryType::Device);
  for (int i = 0; i < count; ++i) {
    auto b = std::make_unique<MetalComputeBackend>(ids.next_as<DeviceID>(), i);
    if (!b->on_gpu()) {
      rep.note += (rep.note.empty() ? "" : "; ") + ("device " + std::to_string(i) + ": " + b->unavailable());
      continue;
    }
    if (!provided_memory && !memory.has_provider(MemoryType::Device) && b->gpu() && b->gpu()->device) {
      memory.register_provider(std::make_unique<MetalDeviceMemoryProvider>(host, b->gpu()->device, b->capacity().memory));
      provided_memory = true;
    }
    rep.devices.push_back(b->name() + " (" + std::to_string(b->capacity().lanes) + " lanes, " +
                          std::to_string(b->capacity().memory >> 20) + " MiB)");
    devices.add(std::move(b));
  }
  if (rep.devices.empty() && rep.note.empty()) rep.note = "Metal is present but registered no device";
  return rep;
}

#else  // not Darwin

struct MetalComputeBackend::Gpu { void* device = nullptr; };

MetalComputeBackend::MetalComputeBackend(const DeviceID& id, int ordinal) : id_(id) {
  (void)ordinal;
  unavailable_ = "Apple Metal runtime is only available on Darwin/macOS";
}
MetalComputeBackend::~MetalComputeBackend() = default;
bool MetalComputeBackend::supports(const Operation&) const { return false; }
Result<void> MetalComputeBackend::register_kernel(const std::string& name, CustomKernel k) {
  hal::LockGuard g(mu_);
  kernels_[fnv1a_hash(name)] = std::move(k);
  return Ok();
}
uint32_t MetalComputeBackend::busy_lanes() const { return busy_lanes_.load(); }
Result<DeviceMemoryStats> MetalComputeBackend::memory_stats() const {
  return Result<DeviceMemoryStats>(Status::Unsupported, "no Metal runtime on this host");
}
Result<void> MetalComputeBackend::execute(const Operation&, MemoryFabric&, uint32_t) {
  return Result<void>(Status::Unsupported, "no Metal device: " + unavailable_);
}
AcceleratorReport register_metal_devices(DeviceFabric&, MemoryFabric&, hal::Host&, IdGenerator&) {
  AcceleratorReport rep;
  rep.note = "Metal compute runtime is only available on Darwin/macOS";
  return rep;
}

#endif

}  // namespace aios::compute
