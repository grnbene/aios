#include <cstdlib>
// Accelerator devices through OpenCL, bound at runtime with dlopen so the
// kernel has no link-time dependency on any vendor. A GPU, an NPU or an
// OpenCL CPU runtime all appear as Devices; the first one also provides
// MemoryType::Device: a buffer on the accelerator mirrored by a host mapping,
// kept coherent by generation (upload before compute, download after).
#include "aios/compute/device.hpp"
#include "aios/compute/gpu_telemetry.hpp"
#include <cstdio>
#include "aios/hal/sync.hpp"
#include <cstring>
#include <atomic>
#include <unordered_map>
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

namespace aios {
namespace {

struct CL {
  void* lib = nullptr;
  decltype(&clGetPlatformIDs) GetPlatformIDs;
  decltype(&clGetPlatformInfo) GetPlatformInfo;
  decltype(&clGetDeviceIDs) GetDeviceIDs;
  decltype(&clGetDeviceInfo) GetDeviceInfo;
  decltype(&clCreateContext) CreateContext;
  decltype(&clCreateCommandQueue) CreateCommandQueue;
  decltype(&clCreateBuffer) CreateBuffer;
  decltype(&clReleaseMemObject) ReleaseMemObject;
  decltype(&clEnqueueWriteBuffer) EnqueueWriteBuffer;
  decltype(&clEnqueueReadBuffer) EnqueueReadBuffer;
  decltype(&clEnqueueCopyBuffer) EnqueueCopyBuffer;
  decltype(&clCreateProgramWithSource) CreateProgramWithSource;
  decltype(&clBuildProgram) BuildProgram;
  decltype(&clGetProgramBuildInfo) GetProgramBuildInfo;
  decltype(&clCreateKernel) CreateKernel;
  decltype(&clSetKernelArg) SetKernelArg;
  decltype(&clEnqueueNDRangeKernel) EnqueueNDRangeKernel;
  decltype(&clFinish) Finish;
  decltype(&clReleaseKernel) ReleaseKernel;
  decltype(&clReleaseProgram) ReleaseProgram;
  bool load() {
    lib = hal::load_library("libOpenCL.so.1");
    if (!lib) lib = hal::load_library("libOpenCL.so");
    if (!lib) return false;
#define L(n) n = reinterpret_cast<decltype(n)>(hal::library_symbol(lib, "cl" #n)); if (!n) return false;
    L(GetPlatformIDs) L(GetPlatformInfo) L(GetDeviceIDs) L(GetDeviceInfo) L(CreateContext) L(CreateCommandQueue) L(CreateBuffer) L(ReleaseMemObject)
    L(EnqueueWriteBuffer) L(EnqueueReadBuffer) L(EnqueueCopyBuffer) L(CreateProgramWithSource) L(BuildProgram) L(GetProgramBuildInfo) L(CreateKernel) L(SetKernelArg)
    L(EnqueueNDRangeKernel) L(Finish) L(ReleaseKernel) L(ReleaseProgram)
#undef L
    return true;
  }
};
CL& cl() { static CL c; return c; }

const char* KERNEL_SOURCE = R"CL(
#pragma OPENCL EXTENSION cl_khr_fp64 : enable
__kernel void fill_f32(__global float* o, float v) { o[get_global_id(0)] = v; }
__kernel void fill_f64(__global double* o, double v) { o[get_global_id(0)] = v; }
__kernel void scale_f32(__global const float* x, __global float* o, float s) { size_t i = get_global_id(0); o[i] = x[i] * s; }
__kernel void scale_f64(__global const double* x, __global double* o, double s) { size_t i = get_global_id(0); o[i] = x[i] * s; }
__kernel void relu_f32(__global const float* x, __global float* o) { size_t i = get_global_id(0); o[i] = x[i] > 0.f ? x[i] : 0.f; }
__kernel void relu_f64(__global const double* x, __global double* o) { size_t i = get_global_id(0); o[i] = x[i] > 0.0 ? x[i] : 0.0; }
__kernel void add_f32(__global const float* x, __global const float* y, __global float* o) { size_t i = get_global_id(0); o[i] = x[i] + y[i]; }
__kernel void add_f64(__global const double* x, __global const double* y, __global double* o) { size_t i = get_global_id(0); o[i] = x[i] + y[i]; }
__kernel void mul_f32(__global const float* x, __global const float* y, __global float* o) { size_t i = get_global_id(0); o[i] = x[i] * y[i]; }
__kernel void mul_f64(__global const double* x, __global const double* y, __global double* o) { size_t i = get_global_id(0); o[i] = x[i] * y[i]; }
__kernel void matmul_f32(__global const float* A, __global const float* B, __global float* C, ulong M, ulong K, ulong N) {
  size_t i = get_global_id(0), j = get_global_id(1); if (i >= M || j >= N) return;
  float acc = 0.f; for (ulong k = 0; k < K; ++k) acc += A[i * K + k] * B[k * N + j]; C[i * N + j] = acc; }
__kernel void matmul_f64(__global const double* A, __global const double* B, __global double* C, ulong M, ulong K, ulong N) {
  size_t i = get_global_id(0), j = get_global_id(1); if (i >= M || j >= N) return;
  double acc = 0.0; for (ulong k = 0; k < K; ++k) acc += A[i * K + k] * B[k * N + j]; C[i * N + j] = acc; }
__kernel void rowsum_f32(__global const float* x, __global float* partial, ulong n, ulong chunk) {
  size_t g = get_global_id(0); ulong a = g * chunk, b = a + chunk; if (b > n) b = n; float s = 0.f; for (ulong i = a; i < b; ++i) s += x[i]; partial[g] = s; }
__kernel void softmax_rows_f32(__global const float* x, __global float* o, ulong last) {
  size_t r = get_global_id(0); __global const float* xr = x + r * last; __global float* orow = o + r * last;
  float m = xr[0]; for (ulong i = 1; i < last; ++i) m = fmax(m, xr[i]);
  float s = 0.f; for (ulong i = 0; i < last; ++i) { orow[i] = exp(xr[i] - m); s += orow[i]; }
  for (ulong i = 0; i < last; ++i) orow[i] /= s; }
__kernel void normalize_rows_f32(__global const float* x, __global float* o, ulong last) {
  size_t r = get_global_id(0); __global const float* xr = x + r * last; __global float* orow = o + r * last;
  float s = 0.f; for (ulong i = 0; i < last; ++i) s += xr[i];
  for (ulong i = 0; i < last; ++i) orow[i] = s != 0.f ? xr[i] / s : 0.f; }
__kernel void histogram_u8(__global const uchar* x, __global uint* h, ulong n, ulong chunk) {
  size_t g = get_global_id(0); ulong a = g * chunk, b = a + chunk; if (b > n) b = n;
  for (ulong i = a; i < b; ++i) atomic_inc(&h[x[i]]); }
)CL";

// Device memory this backend made: the buffer, and the context it lives in.
// Its address in Mapping::device_owner says a device_ctx is one of these and
// not another backend's (CUDA and Vulkan provide Device memory too).
char kOpenClOwner;
struct DeviceBuffer { cl_mem mem; uint64_t size; cl_context ctx; };

class OpenCLDevice final : public Device {
 public:
  OpenCLDevice(const DeviceID& id, cl_device_id dev, cl_context ctx, cl_command_queue q, std::string name, DeviceType type, DeviceCapacity cap, bool fp64)
      : id_(id), dev_(dev), ctx_(ctx), q_(q), name_(std::move(name)), type_(type), cap_(cap), fp64_(fp64) {}
  ~OpenCLDevice() override { for (auto& [n, k] : kernels_) cl().ReleaseKernel(k); if (prog_) cl().ReleaseProgram(prog_); }

  Result<void> build() {
    cl_int err;
    const char* src = KERNEL_SOURCE;
    prog_ = cl().CreateProgramWithSource(ctx_, 1, &src, nullptr, &err);
    if (err != CL_SUCCESS) return Result<void>(Status::IoError, "clCreateProgramWithSource " + std::to_string(err));
    err = cl().BuildProgram(prog_, 1, &dev_, fp64_ ? "" : "-DNO_FP64", nullptr, nullptr);
    if (err != CL_SUCCESS) {
      size_t n = 0; cl().GetProgramBuildInfo(prog_, dev_, CL_PROGRAM_BUILD_LOG, 0, nullptr, &n);
      std::string log(n, '\0'); cl().GetProgramBuildInfo(prog_, dev_, CL_PROGRAM_BUILD_LOG, n, log.data(), nullptr);
      return Result<void>(Status::IoError, "OpenCL program build failed: " + log.substr(0, 400));
    }
    return Ok();
  }
  cl_kernel kernel(const std::string& name) {
    auto it = kernels_.find(name);
    if (it != kernels_.end()) return it->second;
    cl_int err; cl_kernel k = cl().CreateKernel(prog_, name.c_str(), &err);
    if (err != CL_SUCCESS) return nullptr;
    kernels_[name] = k; return k;
  }

  const DeviceID& id() const override { return id_; }
  DeviceType type() const override { return type_; }
  const std::string& name() const override { return name_; }
  const DeviceCapacity& capacity() const override { return cap_; }
  uint32_t busy_lanes() const override { return busy_.load(); }
  // OpenCL has no portable "how much is in use" query. On an NVIDIA card the
  // PCI bus id names the same silicon the driver's management library sees,
  // and that is where the used/free numbers — and the other tenants — come from.
  void set_pci_bus_id(std::string pci) { pci_ = std::move(pci); }
  Result<DeviceMemoryStats> memory_stats() const override {
    if (!pci_.empty()) {
      auto s = compute::nvidia_memory_by_pci(pci_);
      if (s.ok()) return s;
    }
    return Device::memory_stats();
  }
  bool supports(const Operation& op) const override {
    switch (op.code) {
      case OpCode::Nop: case OpCode::Copy: case OpCode::Fill: case OpCode::Add: case OpCode::Mul: case OpCode::Scale: case OpCode::Relu: case OpCode::MatMul: case OpCode::ReduceSum: case OpCode::Softmax: case OpCode::Normalize: case OpCode::Histogram: break;
      default: return false;
    }
    for (auto* list : {&op.inputs, &op.outputs}) for (auto& t : *list) {
      if (t.dtype == DType::F64 && !fp64_) return false;
      if (t.dtype == DType::I32 || t.dtype == DType::I64) return false;
      if (t.dtype == DType::U8 && op.code != OpCode::Histogram) return false;
    }
    return true;
  }
  Result<void> register_kernel(const std::string&, CustomKernel) override { return Result<void>(Status::Unsupported, "custom host kernels do not run on an accelerator"); }

  // Operand staging: a Device-type memory object has a persistent buffer kept
  // coherent by generation; anything else gets a temporary buffer for the call.
  struct Staged { cl_mem mem; bool temp; MemoryID id; uint64_t size; uint8_t* host; bool output; };
  Result<Staged> stage(MemoryFabric& fabric, const Tensor& t, bool output) {
    const MemoryObject* o = fabric.lookup(t.memory);
    if (!o) return Result<Staged>(Status::NotFound, "memory " + t.memory.short_str());
    if (t.offset + t.bytes() > o->size) return Result<Staged>(Status::Invalid, "tensor exceeds its memory object");
    Staged s{nullptr, false, t.memory, o->size, static_cast<uint8_t*>(o->base()), output};
    cl_int err = CL_SUCCESS;
    const auto* own = o->type == MemoryType::Device && o->mapping.device_owner == &kOpenClOwner
                          ? static_cast<const DeviceBuffer*>(o->mapping.device_ctx) : nullptr;
    if (own && own->ctx == ctx_) {
      s.mem = own->mem;
      hal::LockGuard g(mu_);
      auto it = uploaded_.find(t.memory);
      if (it == uploaded_.end() || it->second != o->generation) {
        err = cl().EnqueueWriteBuffer(q_, s.mem, CL_TRUE, 0, o->size, o->base(), 0, nullptr, nullptr);
        if (err != CL_SUCCESS) return Result<Staged>(Status::IoError, "upload " + std::to_string(err));
        uploaded_[t.memory] = o->generation;
      }
    } else {
      s.temp = true;
      s.mem = cl().CreateBuffer(ctx_, CL_MEM_READ_WRITE, o->size, nullptr, &err);
      if (err != CL_SUCCESS) return Result<Staged>(Status::Exhausted, "device buffer " + std::to_string(err));
      err = cl().EnqueueWriteBuffer(q_, s.mem, CL_TRUE, 0, o->size, o->base(), 0, nullptr, nullptr);
      if (err != CL_SUCCESS) { cl().ReleaseMemObject(s.mem); return Result<Staged>(Status::IoError, "upload " + std::to_string(err)); }
    }
    return s;
  }
  void unstage(MemoryFabric& fabric, std::vector<Staged>& all) {
    for (auto& s : all) {
      if (s.output) {
        cl().EnqueueReadBuffer(q_, s.mem, CL_TRUE, 0, s.size, s.host, 0, nullptr, nullptr);
        fabric.touch(s.id);
        if (!s.temp) { hal::LockGuard g(mu_); const MemoryObject* o = fabric.lookup(s.id); if (o) uploaded_[s.id] = o->generation; }
      }
      if (s.temp) cl().ReleaseMemObject(s.mem);
    }
  }

  Result<void> execute(const Operation& op, MemoryFabric& fabric, uint32_t lanes) override {
    struct Busy { std::atomic<uint32_t>& b; uint32_t n; Busy(std::atomic<uint32_t>& x, uint32_t k) : b(x), n(k) { b += n; } ~Busy() { b -= n; } } busy(busy_, std::max<uint32_t>(1, lanes));
    hal::LockGuard ql(queue_mu_);
    std::vector<Staged> st;
    auto cleanup = [&]() { for (auto& s : st) if (s.temp) cl().ReleaseMemObject(s.mem); };
    for (auto& t : op.inputs) { auto s = stage(fabric, t, false); if (!s.ok()) { cleanup(); return s.error(); } st.push_back(*s); }
    size_t nin = st.size();
    for (auto& t : op.outputs) {
      // an output that is also an input shares the staged buffer
      bool shared = false;
      for (size_t i = 0; i < nin; ++i) if (st[i].id == t.memory) { Staged c = st[i]; c.output = true; c.temp = false; st.push_back(c); st[i].output = false; shared = true; break; }
      if (!shared) { auto s = stage(fabric, t, true); if (!s.ok()) { cleanup(); return s.error(); } st.push_back(*s); }
    }
    auto r = run(op, st, nin);
    cl().Finish(q_);
    if (r.ok()) unstage(fabric, st); else cleanup();
    return r;
  }

 private:
  std::string pci_;
  static std::string dt(DType d) { return d == DType::F64 ? "f64" : "f32"; }
  Result<void> launch(cl_kernel k, size_t dims, const size_t* global) {
    cl_int err = cl().EnqueueNDRangeKernel(q_, k, (cl_uint)dims, nullptr, global, nullptr, 0, nullptr, nullptr);
    if (err != CL_SUCCESS) return Result<void>(Status::IoError, "kernel launch failed: " + std::to_string(err));
    return Ok();
  }
  Result<void> arg_buf(cl_kernel k, cl_uint i, cl_mem m) { if (cl().SetKernelArg(k, i, sizeof(cl_mem), &m) != CL_SUCCESS) return Result<void>(Status::IoError, "kernel arg"); return Ok(); }
  template <typename T> Result<void> arg_val(cl_kernel k, cl_uint i, T v) { if (cl().SetKernelArg(k, i, sizeof(T), &v) != CL_SUCCESS) return Result<void>(Status::IoError, "kernel arg"); return Ok(); }
  // Buffers are staged whole; tensor offsets are applied through sub-ranges only when zero (phase 1).
  Result<void> need_zero_offsets(const Operation& op) { for (auto* l : {&op.inputs, &op.outputs}) for (auto& t : *l) if (t.offset) return Result<void>(Status::Unsupported, "accelerator path needs zero tensor offsets"); return Ok(); }

  Result<void> run(const Operation& op, std::vector<Staged>& st, size_t nin) {
    AIOS_TRY(need_zero_offsets(op));
    auto in = [&](size_t i) { return st[i].mem; };
    auto out = [&](size_t i) { return st[nin + i].mem; };
    switch (op.code) {
      case OpCode::Nop: return Ok();
      case OpCode::Copy: {
        if (op.inputs.size() < 1 || op.outputs.size() < 1 || op.inputs[0].bytes() != op.outputs[0].bytes()) return Result<void>(Status::Invalid, "copy shapes");
        if (cl().EnqueueCopyBuffer(q_, in(0), out(0), 0, 0, op.outputs[0].bytes(), 0, nullptr, nullptr) != CL_SUCCESS) return Result<void>(Status::IoError, "copy");
        return Ok();
      }
      case OpCode::Fill: {
        if (op.outputs.empty()) return Result<void>(Status::Invalid, "fill needs an output");
        cl_kernel k = kernel("fill_" + dt(op.outputs[0].dtype)); if (!k) return Result<void>(Status::Unsupported, "fill dtype");
        AIOS_TRY(arg_buf(k, 0, out(0)));
        if (op.outputs[0].dtype == DType::F64) AIOS_TRY(arg_val(k, 1, (double)op.scalar)); else AIOS_TRY(arg_val(k, 1, (float)op.scalar));
        size_t n = op.outputs[0].elements(); return launch(k, 1, &n);
      }
      case OpCode::Scale: case OpCode::Relu: {
        if (op.inputs.size() < 1 || op.outputs.size() < 1 || op.inputs[0].elements() != op.outputs[0].elements()) return Result<void>(Status::Invalid, "shape mismatch");
        cl_kernel k = kernel(std::string(op.code == OpCode::Scale ? "scale_" : "relu_") + dt(op.outputs[0].dtype)); if (!k) return Result<void>(Status::Unsupported, "dtype");
        AIOS_TRY(arg_buf(k, 0, in(0))); AIOS_TRY(arg_buf(k, 1, out(0)));
        if (op.code == OpCode::Scale) { if (op.outputs[0].dtype == DType::F64) AIOS_TRY(arg_val(k, 2, (double)op.scalar)); else AIOS_TRY(arg_val(k, 2, (float)op.scalar)); }
        size_t n = op.outputs[0].elements(); return launch(k, 1, &n);
      }
      case OpCode::Add: case OpCode::Mul: {
        if (op.inputs.size() < 2 || op.outputs.size() < 1 || op.inputs[0].elements() != op.outputs[0].elements() || op.inputs[1].elements() != op.outputs[0].elements()) return Result<void>(Status::Invalid, "shape mismatch");
        cl_kernel k = kernel(std::string(op.code == OpCode::Add ? "add_" : "mul_") + dt(op.outputs[0].dtype)); if (!k) return Result<void>(Status::Unsupported, "dtype");
        AIOS_TRY(arg_buf(k, 0, in(0))); AIOS_TRY(arg_buf(k, 1, in(1))); AIOS_TRY(arg_buf(k, 2, out(0)));
        size_t n = op.outputs[0].elements(); return launch(k, 1, &n);
      }
      case OpCode::MatMul: {
        if (op.inputs.size() < 2 || op.outputs.size() < 1) return Result<void>(Status::Invalid, "matmul needs 2 inputs");
        const Tensor& A = op.inputs[0]; const Tensor& B = op.inputs[1]; const Tensor& C = op.outputs[0];
        if (A.rank() != 2 || B.rank() != 2 || C.rank() != 2 || B.shape[0] != A.shape[1] || C.shape[0] != A.shape[0] || C.shape[1] != B.shape[1]) return Result<void>(Status::Invalid, "matmul shapes");
        cl_kernel k = kernel("matmul_" + dt(C.dtype)); if (!k) return Result<void>(Status::Unsupported, "dtype");
        AIOS_TRY(arg_buf(k, 0, in(0))); AIOS_TRY(arg_buf(k, 1, in(1))); AIOS_TRY(arg_buf(k, 2, out(0)));
        AIOS_TRY(arg_val<cl_ulong>(k, 3, A.shape[0])); AIOS_TRY(arg_val<cl_ulong>(k, 4, A.shape[1])); AIOS_TRY(arg_val<cl_ulong>(k, 5, B.shape[1]));
        size_t g[2] = {A.shape[0], B.shape[1]}; return launch(k, 2, g);
      }
      case OpCode::ReduceSum: {
        if (op.inputs.size() < 1 || op.outputs.size() < 1 || op.inputs[0].dtype != DType::F32) return Result<void>(Status::Unsupported, "reduce-sum f32");
        uint64_t n = op.inputs[0].elements(); size_t groups = std::min<uint64_t>(256, std::max<uint64_t>(1, n)); uint64_t chunk = (n + groups - 1) / groups;
        cl_int err; cl_mem partial = cl().CreateBuffer(ctx_, CL_MEM_READ_WRITE, groups * 4, nullptr, &err); if (err != CL_SUCCESS) return Result<void>(Status::Exhausted, "buffer");
        cl_kernel k = kernel("rowsum_f32");
        AIOS_TRY(arg_buf(k, 0, in(0))); AIOS_TRY(arg_buf(k, 1, partial)); AIOS_TRY(arg_val<cl_ulong>(k, 2, n)); AIOS_TRY(arg_val<cl_ulong>(k, 3, chunk));
        auto r = launch(k, 1, &groups);
        if (r.ok()) {
          std::vector<float> p(groups); cl().EnqueueReadBuffer(q_, partial, CL_TRUE, 0, groups * 4, p.data(), 0, nullptr, nullptr);
          double s = 0; for (float f : p) s += f; float fs = (float)s;
          cl().EnqueueWriteBuffer(q_, out(0), CL_TRUE, 0, 4, &fs, 0, nullptr, nullptr);
        }
        cl().ReleaseMemObject(partial); return r;
      }
      case OpCode::Softmax: case OpCode::Normalize: {
        if (op.inputs.size() < 1 || op.outputs.size() < 1 || op.inputs[0].dtype != DType::F32) return Result<void>(Status::Unsupported, "f32 only");
        const Tensor& X = op.inputs[0]; uint64_t last = X.shape.empty() ? 0 : X.shape.back(); if (!last) return Ok();
        size_t rows = X.elements() / last;
        cl_kernel k = kernel(op.code == OpCode::Softmax ? "softmax_rows_f32" : "normalize_rows_f32");
        AIOS_TRY(arg_buf(k, 0, in(0))); AIOS_TRY(arg_buf(k, 1, out(0))); AIOS_TRY(arg_val<cl_ulong>(k, 2, last));
        return launch(k, 1, &rows);
      }
      case OpCode::Histogram: {
        if (op.inputs.size() < 1 || op.outputs.size() < 1 || op.inputs[0].dtype != DType::U8 || op.outputs[0].elements() < 256) return Result<void>(Status::Invalid, "histogram: u8 in, f32[256] out");
        uint64_t n = op.inputs[0].elements(); size_t groups = std::min<uint64_t>(64, std::max<uint64_t>(1, n)); uint64_t chunk = (n + groups - 1) / groups;
        std::vector<uint32_t> zero(256, 0); cl_int err;
        cl_mem h = cl().CreateBuffer(ctx_, CL_MEM_READ_WRITE, 256 * 4, nullptr, &err); if (err != CL_SUCCESS) return Result<void>(Status::Exhausted, "buffer");
        cl().EnqueueWriteBuffer(q_, h, CL_TRUE, 0, 256 * 4, zero.data(), 0, nullptr, nullptr);
        cl_kernel k = kernel("histogram_u8");
        AIOS_TRY(arg_buf(k, 0, in(0))); AIOS_TRY(arg_buf(k, 1, h)); AIOS_TRY(arg_val<cl_ulong>(k, 2, n)); AIOS_TRY(arg_val<cl_ulong>(k, 3, chunk));
        auto r = launch(k, 1, &groups);
        if (r.ok()) {
          std::vector<uint32_t> counts(256); cl().EnqueueReadBuffer(q_, h, CL_TRUE, 0, 256 * 4, counts.data(), 0, nullptr, nullptr);
          std::vector<float> f(256); for (int i = 0; i < 256; ++i) f[i] = (float)counts[i];
          cl().EnqueueWriteBuffer(q_, out(0), CL_TRUE, 0, 256 * 4, f.data(), 0, nullptr, nullptr);
        }
        cl().ReleaseMemObject(h); return r;
      }
      default: return Result<void>(Status::Unsupported, std::string("opcode ") + opcode_name(op.code) + " on accelerator");
    }
  }

  DeviceID id_;
  cl_device_id dev_;
  cl_context ctx_;
  cl_command_queue q_;
  std::string name_;
  DeviceType type_;
  DeviceCapacity cap_;
  bool fp64_;
  cl_program prog_ = nullptr;
  std::unordered_map<std::string, cl_kernel> kernels_;
  std::atomic<uint32_t> busy_{0};
  hal::Mutex mu_, queue_mu_;
  std::unordered_map<MemoryID, uint64_t, TypedIdHash<2>> uploaded_;
 public:
  cl_context context() const { return ctx_; }
};

// MemoryType::Device: an accelerator buffer plus a host mirror the fabric reads
// and writes; the device syncs by generation.
class DeviceMemoryProvider final : public MemoryProvider {
 public:
  DeviceMemoryProvider(hal::Host& host, cl_context ctx, uint64_t capacity) : host_(host), ctx_(ctx), capacity_(capacity) {}
  MemoryType type() const override { return MemoryType::Device; }
  uint64_t capacity() const override { return capacity_; }
  Result<hal::Mapping> allocate(uint64_t size, const MemoryID&) override {
    auto m = host_.map(size, hal::MappingKind::Private);
    if (!m.ok()) return m;
    cl_int err;
    cl_mem mem = cl().CreateBuffer(ctx_, CL_MEM_READ_WRITE, size, nullptr, &err);
    if (err != CL_SUCCESS) { host_.unmap(*m); return Result<hal::Mapping>(Status::Exhausted, "device memory allocation failed (" + std::to_string(err) + ")"); }
    m->device_ctx = new DeviceBuffer{mem, size, ctx_};
    m->device_owner = &kOpenClOwner;
    return m;
  }
  void release(hal::Mapping& m) override {
    if (m.device_ctx && m.device_owner == &kOpenClOwner) { auto* b = static_cast<DeviceBuffer*>(m.device_ctx); cl().ReleaseMemObject(b->mem); delete b; m.device_ctx = nullptr; m.device_owner = nullptr; }
    host_.unmap(m);
  }
 private:
  hal::Host& host_;
  cl_context ctx_;
  uint64_t capacity_;
};

std::string dev_str(cl_device_id d, cl_device_info what) {
  size_t n = 0; cl().GetDeviceInfo(d, what, 0, nullptr, &n);
  std::string s(n, '\0'); cl().GetDeviceInfo(d, what, n, s.data(), nullptr);
  while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
  return s;
}

}  // namespace

AcceleratorReport register_opencl_devices(DeviceFabric& devices, MemoryFabric& memory, hal::Host& host, IdGenerator& ids) {
  AcceleratorReport rep;
  // AIOS_NO_ACCEL skips accelerator discovery entirely. A statically linked build skips it by itself: it cannot
  // load the host's OpenCL runtime (hal::load_library says why).
  if (const char* e = getenv("AIOS_NO_ACCEL"); e && e[0] && e[0] != '0') { rep.note = "accelerators disabled (AIOS_NO_ACCEL)"; return rep; }
  if (const char* e = getenv("AIOS_NO_OPENCL"); e && e[0] && e[0] != '0') { rep.note = "OpenCL disabled (AIOS_NO_OPENCL)"; return rep; }
  if (!cl().load()) {
    const std::string why = hal::library_error();
    rep.note = why.find("statically linked") != std::string::npos ? "OpenCL not loaded: " + why : "no OpenCL runtime on this host";
    return rep;
  }
  cl_uint np = 0;
  if (cl().GetPlatformIDs(0, nullptr, &np) != CL_SUCCESS || np == 0) { rep.note = "OpenCL runtime present, no platforms"; return rep; }
  std::vector<cl_platform_id> plats(np);
  cl().GetPlatformIDs(np, plats.data(), nullptr);
  bool provided_memory = false;
  for (auto p : plats) {
    cl_uint nd = 0;
    if (cl().GetDeviceIDs(p, CL_DEVICE_TYPE_ALL, 0, nullptr, &nd) != CL_SUCCESS || nd == 0) continue;
    std::vector<cl_device_id> devs(nd);
    cl().GetDeviceIDs(p, CL_DEVICE_TYPE_ALL, nd, devs.data(), nullptr);
    for (auto d : devs) {
      cl_device_type t = 0; cl().GetDeviceInfo(d, CL_DEVICE_TYPE, sizeof t, &t, nullptr);
      cl_uint cus = 1; cl().GetDeviceInfo(d, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof cus, &cus, nullptr);
      cl_ulong gmem = 0; cl().GetDeviceInfo(d, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof gmem, &gmem, nullptr);
      cl_uint mhz = 0; cl().GetDeviceInfo(d, CL_DEVICE_MAX_CLOCK_FREQUENCY, sizeof mhz, &mhz, nullptr);
      std::string ext = dev_str(d, CL_DEVICE_EXTENSIONS);
      bool fp64 = ext.find("cl_khr_fp64") != std::string::npos;
      DeviceType type = (t & CL_DEVICE_TYPE_GPU) ? DeviceType::GPU : (t & CL_DEVICE_TYPE_ACCELERATOR) ? DeviceType::NPU : DeviceType::CPU;
      cl_int err;
      std::string dev_name = dev_str(d, CL_DEVICE_NAME);
      cl_context ctx = cl().CreateContext(nullptr, 1, &d, nullptr, nullptr, &err);
      if (err != CL_SUCCESS) { rep.note += dev_name + ": clCreateContext " + std::to_string(err) + " (a full card refuses a context); "; continue; }
      cl_command_queue q = cl().CreateCommandQueue(ctx, d, 0, &err);
      if (err != CL_SUCCESS) { rep.note += dev_name + ": clCreateCommandQueue " + std::to_string(err) + "; "; continue; }
      DeviceCapacity cap;
      cap.lanes = cus; cap.memory = gmem; cap.native_memory = MemoryType::Device;
      cap.flops_per_second = uint64_t(cus) * uint64_t(mhz ? mhz : 1000) * 1000000ull * (type == DeviceType::GPU ? 64 : 8);
      std::string name = std::string(type == DeviceType::GPU ? "gpu:" : type == DeviceType::NPU ? "npu:" : "ocl-cpu:") + dev_str(d, CL_DEVICE_NAME);
      auto dev = std::make_unique<OpenCLDevice>(ids.next_as<DeviceID>(), d, ctx, q, name, type, cap, fp64);
      if (ext.find("cl_nv_device_attribute_query") != std::string::npos) {
        cl_uint bus = 0, slot = 0;   // CL_DEVICE_PCI_BUS_ID_NV, CL_DEVICE_PCI_SLOT_ID_NV
        if (cl().GetDeviceInfo(d, 0x4008, sizeof bus, &bus, nullptr) == CL_SUCCESS &&
            cl().GetDeviceInfo(d, 0x4009, sizeof slot, &slot, nullptr) == CL_SUCCESS) {
          char pci[32];
          std::snprintf(pci, sizeof pci, "0000:%02x:%02x.%x", bus & 0xff, (slot >> 3) & 0x1f, slot & 0x7);
          dev->set_pci_bus_id(pci);
        }
      }
      auto b = dev->build();
      if (!b.ok()) { rep.note += name + ": " + b.error().str() + "; "; continue; }
      if (!provided_memory && !memory.has_provider(MemoryType::Device)) { memory.register_provider(std::make_unique<DeviceMemoryProvider>(host, ctx, gmem)); provided_memory = true; }
      rep.devices.push_back(name + " (" + std::to_string(cus) + " units, " + std::to_string(gmem >> 20) + " MiB, " + (fp64 ? "fp64" : "fp32") + ")");
      devices.add(std::move(dev));
    }
  }
  if (rep.devices.empty() && rep.note.empty()) rep.note = "OpenCL runtime present, no usable devices";
  return rep;
}

}  // namespace aios
