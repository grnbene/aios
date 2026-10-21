#include <atomic>
#include "aios/intelligence/world_model.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
#include "aios/core/ids.hpp"
#include "aios/core/result.hpp"
#include "aios/kernel/surface.hpp"

namespace aios::intelligence {

// Constructed on FIRST USE, not at static-init.
//
// These two models are 17 MiB of heap between them — the predictor's weights
// and its two AdamW moment buffers are 13.4 MiB on their own. As file-scope
// objects they were allocated by the init_array before kernel_main had decided
// anything, on a bare-metal machine whose whole heap is 64 MiB and which then
// hands 32 MiB of that to metal_process_init. A machine that never asks the
// world model anything should not pay for it, and one that does can pay when it
// asks. -fno-threadsafe-statics is set for the metal build, so a function-local
// static here costs an initialised flag and a branch, not a guard acquire.
//
// The addresses stay stable, which SYS_JEPA_SHM depends on.
JEPAWorldModel* get_global_jepa_world_model() {
  static JEPAWorldModel model;
  return &model;
}
ExecutionTraceLogger* get_global_trace_logger() {
  static ExecutionTraceLogger logger;
  return &logger;
}
SharedStateVectorRegion* get_global_shared_state_region() {
  static SharedStateVectorRegion region;
  return &region;
}

void update_shared_state_region(const SubstrateState& s_t, const std::vector<float>& a_t, const SubstrateState& s_t1) {
  // Seqlock writer. The odd count must be visible BEFORE any of the data stores
  // that follow, or a reader on a weakly ordered CPU (AArch64) can see new data
  // under the old, even count and accept a torn copy: the relaxed increment has
  // to be followed by a release fence (store-store order), not just preceded by
  // nothing.
  get_global_shared_state_region()->sequence.fetch_add(1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  get_global_shared_state_region()->timestamp_ms = s_t.timestamp_ms;
  for (size_t i = 0; i < kSubstrateStateDim && i < s_t.vector.size(); ++i) {
    get_global_shared_state_region()->state_vector[i] = s_t.vector[i];
  }
  for (size_t i = 0; i < kActionVectorDim && i < a_t.size(); ++i) {
    get_global_shared_state_region()->action_vector[i] = a_t[i];
  }
  for (size_t i = 0; i < kSubstrateStateDim && i < s_t1.vector.size(); ++i) {
    get_global_shared_state_region()->predicted_next_state[i] = s_t1.vector[i];
  }
  get_global_shared_state_region()->sequence.fetch_add(1, std::memory_order_release);
}

SubstrateState::SubstrateState() : timestamp_ms(0) {
  vector.resize(kSubstrateStateDim, 0.0f);
}

SubstrateState SubstrateState::encode_context(const StateObject& context) {
  SubstrateState state;
  state.entity_id = context.id;
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  state.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

  // Pseudo-random deterministic feature projection from context bytes
  uint64_t hash = context.id.hi ^ context.id.lo ^ state.timestamp_ms;
  std::mt19937 rng(static_cast<uint32_t>(hash));
  std::normal_distribution<float> dist(0.0f, 0.1f);

  for (size_t i = 0; i < kSubstrateStateDim; ++i) {
    state.vector[i] = dist(rng);
  }

  // Normalize state vector
  float norm = 0.0f;
  for (float v : state.vector) norm += v * v;
  norm = std::sqrt(std::max(norm, 1e-6f));
  for (float& v : state.vector) v /= norm;

  return state;
}

static inline float freestanding_sin(float x) {
  constexpr float pi = 3.1415926535f;
  while (x > pi) x -= 2.0f * pi;
  while (x < -pi) x += 2.0f * pi;
  if (x < 0.0f) return -freestanding_sin(-x);
  return (16.0f * x * (pi - x)) / (5.0f * pi * pi - 4.0f * x * (pi - x));
}

static inline float freestanding_cos(float x) {
  constexpr float half_pi = 1.57079632679f;
  return freestanding_sin(x + half_pi);
}

// ---- the substrate's instruments and levers ---------------------------------
// Registered by whoever actually owns the hardware. The core never guesses.
namespace {
TelemetryProvider g_telemetry = nullptr;
ActuationSink g_actuation = nullptr;
SubstrateSettle g_settle = nullptr;
}  // namespace

void set_telemetry_provider(TelemetryProvider p) { g_telemetry = p; }
void set_actuation_sink(ActuationSink s) { g_actuation = s; }
namespace { SubstratePressure g_pressure = nullptr; }
void set_substrate_pressure_hook(SubstratePressure p) { g_pressure = p; }
void set_settle_pressure(int level) { if (g_pressure) g_pressure(level); }
bool pressure_hook_is_live() { return g_pressure != nullptr; }
bool telemetry_is_live() { return g_telemetry != nullptr; }
bool actuation_is_live() { return g_actuation != nullptr; }
void set_substrate_settle(SubstrateSettle s) { g_settle = s; }
bool settle_is_live() { return g_settle != nullptr; }
bool substrate_settle() {
  if (!g_settle) return false;
  g_settle();
  return true;
}

namespace { SubstrateTelemetry g_last_telemetry; }
// The most recent reading, WITHOUT taking another. Every provider here
// differences against its own previous call, so a second read restarts the
// interval and answers 0 — which is how a diagnostic printed "raw 0" while the
// number it was diagnosing was 1.0075.
SubstrateTelemetry last_substrate_telemetry() { return g_last_telemetry; }

SubstrateTelemetry read_substrate_telemetry() {
  if (g_telemetry) {
    SubstrateTelemetry t = g_telemetry();
    t.live = true;
    g_last_telemetry = t;
    return t;
  }
  // No provider: a deterministic synthetic driver, flagged as NOT live so no
  // caller can mistake it for a measurement.
  static uint64_t step = 0;
  ++step;
  SubstrateTelemetry t;
  t.cpu_load = 0.15f + 0.10f * freestanding_sin(step * 0.05f);
  t.nic_ring_depth = 0.20f + 0.15f * freestanding_cos(step * 0.08f);
  t.ctx_switch_rate = 0.05f + 0.02f * freestanding_sin(step * 0.1f);
  t.mem_pressure = 0.30f + 0.05f * freestanding_sin(step * 0.02f);
  t.compute_occupancy = 0.0f;
  t.timestamp_ms = step * 10;
  t.live = false;
  return t;
}

// a_t[0] asks for a scheduler quantum. tanh-squash it into 1..16 ticks so the
// policy head cannot ask for something the scheduler would choke on; a_t[1] is
// a gate on NIC prefetch. The remaining 126 dims are unbound — the model can
// use them as latent capacity, but nothing downstream reads them yet.
SubstrateActuation decode_action(const std::vector<float>& a_t) {
  SubstrateActuation act;
  if (a_t.empty()) return act;
  float q = a_t[0];
  if (q < -1.0f) q = -1.0f;
  if (q > 1.0f) q = 1.0f;
  float scaled = 8.5f + 7.5f * q;                   // [1, 16]
  uint32_t ticks = (uint32_t)(scaled + 0.5f);
  if (ticks < 1) ticks = 1;
  if (ticks > 16) ticks = 16;
  act.scheduler_slice_ticks = ticks;
  act.nic_prefetch_enabled = a_t.size() > 1 && a_t[1] > 0.0f;

  // a_t[1] also carries HOW MANY buffers to keep posted, not only whether to
  // keep more than the minimum. 4 with the window closed, up to the ring's 32
  // with it open, so the driver has a number to act on and the reward has
  // something to move.
  if (a_t.size() > 1) {
    float w = a_t[1];
    if (w < -1.0f) w = -1.0f;
    if (w > 1.0f) w = 1.0f;
    const float win = 18.0f + 14.0f * w;            // [4, 32]
    uint32_t buffers = (uint32_t)(win + 0.5f);
    if (buffers < 4) buffers = 4;
    if (buffers > 32) buffers = 32;
    act.nic_refill_window = buffers;
  }

  // a_t[2]: which of the machine's scenes is on its own display. Quantised over
  // the whole range rather than thresholded, so the policy can reach all of
  // them and not only the two ends.
  if (a_t.size() > 2) {
    float d = a_t[2];
    if (d < -1.0f) d = -1.0f;
    if (d > 1.0f) d = 1.0f;
    const float pick = (d + 1.0f) * 0.5f * 3.999f;  // [0, 4)
    uint32_t scene = (uint32_t)pick;
    if (scene > 3) scene = 3;
    act.display_scene = scene;
  }

  // a_t[3]: the quota, as a percentage of measured device time. Never below 10,
  // because a quota that starves an entity to a stop is not a load correction.
  if (a_t.size() > 3) {
    float qq = a_t[3];
    if (qq < -1.0f) qq = -1.0f;
    if (qq > 1.0f) qq = 1.0f;
    const float pct = 55.0f + 45.0f * qq;           // [10, 100]
    uint32_t q2 = (uint32_t)(pct + 0.5f);
    if (q2 < 10) q2 = 10;
    if (q2 > 100) q2 = 100;
    act.compute_quota_pct = q2;
  }
  return act;
}

bool apply_action(const std::vector<float>& a_t, SubstrateActuation* applied) {
  SubstrateActuation act = decode_action(a_t);
  if (applied) *applied = act;
  if (!g_actuation) return false;
  return g_actuation(act);
}

// The measuring instrument, and now identical in KIND to `host jepa prove`:
// context switches counted against the work that actually completed, with the
// workload held fixed.
//
// It used to be the DROP in that ratio from s_t to s_t1. That was wrong once a
// settler existed: s_t is read on an idle machine and s_t1 after the workload
// ran, so the ratio always rises and the reward was negative for every action
// regardless of what it chose — measured on metal, mean -0.36, best -0.267,
// with no action ever scoring above another. The comparison was idle-vs-busy,
// not action-vs-action.
//
// Because the settler runs the SAME workload on every transition (the same
// thing that makes `prove` a fair table), the comparable quantity is simply the
// switch cost of the settled interval itself: switches (dim 2) over the work
// that ran (dim 0), negated so that fewer switches per unit work is a higher
// reward. That is the 460 -> 30 axis, one transition at a time.
float measured_reward(const SubstrateState& s_t, const SubstrateState& s_t1) {
  (void)s_t;   // the pre-action read is the state the policy conditioned on, not the score
  if (s_t1.vector.size() < 8) return 0.0f;
  const float kMinWork = 0.05f;  // below this essentially nothing ran; the ratio is noise
  const float work = s_t1.vector[0] > kMinWork ? s_t1.vector[0] : kMinWork;

  // 1. The switch cost of the settled interval, on the axis `jepa prove`
  //    reports going 460 -> 30. This is the term that was here alone.
  const float switch_cost = s_t1.vector[2] / work;

  // 2. What the interfaces lost. A policy cannot learn to correct packet loss
  //    if nothing it is scored on ever mentions packet loss — no amount of
  //    recorded transitions teaches a lever whose effect the reward cannot see.
  //    Weighted well above the switch term because a dropped frame costs more
  //    than a context switch: loss is measured in [0,1] and a percent of it is
  //    worth about a whole switch-cost unit.
  const float loss_cost = 4.0f * s_t1.vector[6];

  // 3. Runnable work per processor over what the machine has to run it with.
  //    Only counted ABOVE saturation: a busy machine is not a fault, a machine
  //    with more runnable work than places to run it is.
  const float over = s_t1.vector[7] > 0.25f ? (s_t1.vector[7] - 0.25f) : 0.0f;
  const float load_cost = 2.0f * over;

  return -(switch_cost + loss_cost + load_cost);
}

SubstrateState SubstrateState::sample_kernel_telemetry() {
  SubstrateState state;
  state.entity_id = Id128{0x8000, 0x9000};

  static uint64_t step_counter = 0;
  step_counter++;

  SubstrateTelemetry t = read_substrate_telemetry();
  state.timestamp_ms = t.timestamp_ms;

  // Latent dimensions 0..5 ARE the machine's instruments. When a substrate has
  // registered a provider these are measurements; dim 5 records which, so the
  // model itself can tell a measured trace from a synthetic one.
  //   0: cpu load        1: nic rx ring depth     2: context switches / tick
  //   3: heap pressure   4: compute occupancy     5: 1.0 = measured, 0.0 = synthetic
  //   6: packet loss     7: load imbalance
  state.vector[0] = t.cpu_load;
  state.vector[1] = t.nic_ring_depth;
  state.vector[2] = t.ctx_switch_rate;
  state.vector[3] = t.mem_pressure;
  state.vector[4] = t.compute_occupancy;
  state.vector[5] = t.live ? 1.0f : 0.0f;
  // Two more instruments, so the two levers added beside them are judged by
  // something measured rather than by nothing: what the interfaces are losing,
  // and how much runnable work there is per processor.
  state.vector[6] = t.packet_loss;
  state.vector[7] = t.load_imbalance;

  // Dims 8..1023 are latent capacity: no instrument maps to them, so they carry
  // small noise rather than a fabricated reading.
  std::mt19937 rng(static_cast<uint32_t>(state.timestamp_ms ^ step_counter));
  std::normal_distribution<float> dist(0.0f, 0.05f);
  for (size_t i = 8; i < kSubstrateStateDim; ++i) {
    state.vector[i] = dist(rng);
  }

  return state;
}

ExecutionTraceLogger::ExecutionTraceLogger(size_t capacity) : capacity_(capacity) {
  buffer_.reserve(capacity);
}

void ExecutionTraceLogger::log_transition(const SubstrateState& s_t, const std::vector<float>& a_t,
                                          const SubstrateState& s_t1, float reward) {
  hal::LockGuard lock(mu_);
  if (buffer_.size() >= capacity_) {
    buffer_.erase(buffer_.begin());
  }
  TraceSample sample;
  sample.s_t = s_t;
  sample.a_t = a_t;
  if (sample.a_t.size() < kActionVectorDim) {
    sample.a_t.resize(kActionVectorDim, 0.0f);
  }
  sample.s_t1 = s_t1;
  sample.reward = reward;
  buffer_.push_back(sample);
}

std::vector<TraceSample> ExecutionTraceLogger::get_samples(size_t batch_size) const {
  hal::LockGuard lock(mu_);
  if (buffer_.empty()) return {};
  const size_t count = std::min(batch_size, buffer_.size());
  std::vector<TraceSample> out;
  out.reserve(count);
  // Sampled WITH replacement. Drawing without it would cost a shuffle or a
  // visited set per step for a difference that does not matter at these batch
  // sizes, and the learner calls this on every single iteration.
  for (size_t i = 0; i < count; ++i) {
    draw_state_ ^= draw_state_ >> 12;
    draw_state_ ^= draw_state_ << 25;
    draw_state_ ^= draw_state_ >> 27;
    const uint64_t r = draw_state_ * 0x2545F4914F6CDD1Dull;
    out.push_back(buffer_[static_cast<size_t>(r >> 33) % buffer_.size()]);
  }
  return out;
}

std::vector<TraceSample> ExecutionTraceLogger::get_recent(size_t count) const {
  hal::LockGuard lock(mu_);
  if (buffer_.empty()) return {};
  const size_t n = std::min(count, buffer_.size());
  return std::vector<TraceSample>(buffer_.end() - static_cast<ptrdiff_t>(n), buffer_.end());
}

size_t ExecutionTraceLogger::size() const {
  hal::LockGuard lock(mu_);
  return buffer_.size();
}

size_t ExecutionTraceLogger::measured_count() const {
  hal::LockGuard lock(mu_);
  size_t n = 0;
  for (const auto& t : buffer_)
    if (t.s_t.vector.size() > 5 && t.s_t.vector[5] > 0.5f) ++n;
  return n;
}

void ExecutionTraceLogger::clear() {
  hal::LockGuard lock(mu_);
  buffer_.clear();
}

JEPAWorldModel::JEPAWorldModel() {
  size_t in_dim = kSubstrateStateDim + kActionVectorDim; // 1024 + 128 = 1152
  size_t hidden_dim = 512;
  size_t out_dim = kSubstrateStateDim; // 1024

  W1_.resize(hidden_dim * in_dim);
  b1_.resize(hidden_dim, 0.0f);
  W2_.resize(out_dim * hidden_dim);
  b2_.resize(out_dim, 0.0f);

  mW1_.resize(W1_.size(), 0.0f); vW1_.resize(W1_.size(), 0.0f);
  mb1_.resize(b1_.size(), 0.0f); vb1_.resize(b1_.size(), 0.0f);
  mW2_.resize(W2_.size(), 0.0f); vW2_.resize(W2_.size(), 0.0f);
  mb2_.resize(b2_.size(), 0.0f); vb2_.resize(b2_.size(), 0.0f);

  std::mt19937 rng(42);
  float scale1 = std::sqrt(2.0f / static_cast<float>(in_dim));
  std::normal_distribution<float> dist1(0.0f, scale1);
  for (float& w : W1_) w = dist1(rng);

  float scale2 = std::sqrt(2.0f / static_cast<float>(hidden_dim));
  std::normal_distribution<float> dist2(0.0f, scale2);
  for (float& w : W2_) w = dist2(rng);
}


// ---------------------------------------------------------------------------
// Where the predictor's matrix products run.
// ---------------------------------------------------------------------------
// Registered by whoever owns the compute fabric. The core cannot know what this
// machine has under it and does not ask; with nothing registered every product
// below is the scalar C++ it always was, on any host and on bare metal.
namespace {
MatmulOffload g_matmul = nullptr;
const char* g_matmul_what = "scalar CPU (freestanding kernel core)";
MatmulOffloadCount g_matmul_counts;
}  // namespace

void set_matmul_offload(MatmulOffload fn, const char* what) {
  g_matmul = fn;
  g_matmul_what = (fn && what && *what) ? what : "scalar CPU (freestanding kernel core)";
}
bool matmul_offload_is_live() { return g_matmul != nullptr; }
const char* matmul_offload_name() { return g_matmul_what; }
MatmulOffloadCount matmul_offload_counters() { return g_matmul_counts; }

void matmul(const float* A, const float* B, float* C, int M, int K, int N) {
  if (M <= 0 || K <= 0 || N <= 0) return;
  if (g_matmul) {
    if (g_matmul(A, B, C, M, K, N)) {
      ++g_matmul_counts.offloaded;
      g_matmul_counts.macs += (uint64_t)M * (uint64_t)K * (uint64_t)N;
      return;
    }
    // A device that declined has to be done here instead. Silently returning
    // an untouched C would be a wrong answer dressed as an accelerated one.
    ++g_matmul_counts.declined;
  }
  for (int i = 0; i < M; ++i) {
    const float* a = A + (size_t)i * K;
    float* c = C + (size_t)i * N;
    for (int j = 0; j < N; ++j) c[j] = 0.0f;
    for (int k = 0; k < K; ++k) {
      const float av = a[k];
      if (av == 0.0f) continue;
      const float* b = B + (size_t)k * N;
      for (int j = 0; j < N; ++j) c[j] += av * b[j];
    }
  }
}

// t(x) = clamp(x(27+x^2)/(27+9x^2), -1, 1): a rational approximation of tanh.
// The GELU here is x/2 (1 + t(x)) — tanh(x) standing in for tanh(sqrt(2/pi)(x+0.044715x^3)),
// i.e. this is the function the model computes, and its derivative below is
// the exact derivative of THIS function, not of the textbook GELU.
static inline float rational_tanh(float x) {
  const float x_sq = x * x;
  const float t_val = x * (27.0f + x_sq) / (27.0f + 9.0f * x_sq);
  return std::max(-1.0f, std::min(1.0f, t_val));
}
// d/dx of the unclamped ratio is 9(x^2-9)^2 / (27+9x^2)^2; 0 where the clamp holds.
static inline float rational_tanh_grad(float x) {
  const float x_sq = x * x;
  const float raw = x * (27.0f + x_sq) / (27.0f + 9.0f * x_sq);
  if (raw >= 1.0f || raw <= -1.0f) return 0.0f;
  const float num = (x_sq - 9.0f);
  const float den = 27.0f + 9.0f * x_sq;
  return 9.0f * num * num / (den * den);
}
static inline float freestanding_gelu(float x) {
  return x * 0.5f * (1.0f + rational_tanh(x));
}
// d/dx [x/2 (1 + t(x))] = (1 + t(x))/2 + x t'(x)/2, at the PRE-activation x.
static inline float freestanding_gelu_grad(float x) {
  return 0.5f * (1.0f + rational_tanh(x)) + 0.5f * x * rational_tanh_grad(x);
}

SubstrateState JEPAWorldModel::predict_next_state(const SubstrateState& s_t, const std::vector<float>& a_t) {
  hal::LockGuard lock(mu_);
  SubstrateState s_hat;
  s_hat.entity_id = s_t.entity_id;
  s_hat.timestamp_ms = s_t.timestamp_ms + 10;

  size_t in_dim = kSubstrateStateDim + kActionVectorDim;
  size_t hidden_dim = 512;
  size_t out_dim = kSubstrateStateDim;

  std::vector<float> x(in_dim, 0.0f);
  for (size_t i = 0; i < kSubstrateStateDim && i < s_t.vector.size(); ++i) {
    x[i] = s_t.vector[i];
  }
  for (size_t i = 0; i < kActionVectorDim && i < a_t.size(); ++i) {
    x[kSubstrateStateDim + i] = a_t[i];
  }

  // Layer 1: h = GELU(W1 * x + b1)
  std::vector<float> h(hidden_dim, 0.0f);
  for (size_t i = 0; i < hidden_dim; ++i) {
    float sum = b1_[i];
    const float* row = &W1_[i * in_dim];
    for (size_t j = 0; j < in_dim; ++j) {
      sum += row[j] * x[j];
    }
    h[i] = freestanding_gelu(sum);
  }

  // Layer 2: Residual delta = GELU(W2 * h + b2)
  for (size_t i = 0; i < out_dim; ++i) {
    float sum = b2_[i];
    const float* row = &W2_[i * hidden_dim];
    for (size_t j = 0; j < hidden_dim; ++j) {
      sum += row[j] * h[j];
    }
    float delta = freestanding_gelu(sum);
    // Residual Skip Connection: s_hat = s_t + 0.1 * delta
    float base = (i < s_t.vector.size()) ? s_t.vector[i] : 0.0f;
    s_hat.vector[i] = base + 0.1f * delta;
  }

  return s_hat;
}

std::vector<SubstrateState> JEPAWorldModel::predict_trajectory(const SubstrateState& s_0,
                                                               const std::vector<std::vector<float>>& actions) {
  std::vector<SubstrateState> trajectory;
  trajectory.reserve(actions.size());
  SubstrateState current = s_0;

  for (const auto& action : actions) {
    SubstrateState next_s = predict_next_state(current, action);
    trajectory.push_back(next_s);
    current = next_s;
  }
  return trajectory;
}

float JEPAWorldModel::evaluate_loss(const SubstrateState& s_t, const std::vector<float>& a_t,
                                    const SubstrateState& s_t1) {
  SubstrateState s_hat = predict_next_state(s_t, a_t);
  float mse = 0.0f;
  float mean = 0.0f;
  for (size_t i = 0; i < kSubstrateStateDim; ++i) {
    float diff = s_hat.vector[i] - s_t1.vector[i];
    mse += diff * diff;
    mean += s_hat.vector[i];
  }
  mean /= static_cast<float>(kSubstrateStateDim);
  float var = 0.0f;
  for (size_t i = 0; i < kSubstrateStateDim; ++i) {
    float d = s_hat.vector[i] - mean;
    var += d * d;
  }
  var /= static_cast<float>(kSubstrateStateDim);
  float std_dev = std::sqrt(var + 1e-6f);
  float vicreg_var_loss = std::max(0.0f, 1.0f - std_dev);
  std_loss_.store(vicreg_var_loss);

  return (mse / static_cast<float>(kSubstrateStateDim)) + 0.1f * vicreg_var_loss;
}

Result<float> JEPAWorldModel::train_step(const std::vector<TraceSample>& batch, compute::CudaComputeBackend* cuda_backend) {
  hal::LockGuard lock(mu_);
  if (batch.empty()) {
    return last_loss_.load();
  }

  size_t in_dim = kSubstrateStateDim + kActionVectorDim; // 1152
  size_t hidden_dim = 512;
  size_t out_dim = kSubstrateStateDim; // 1024
  float lr = 0.005f;
  float beta1 = 0.9f;
  float beta2 = 0.999f;
  float eps = 1e-8f;
  float weight_decay = 0.0001f;

  std::vector<float> dW1(W1_.size(), 0.0f);
  std::vector<float> db1(b1_.size(), 0.0f);
  std::vector<float> dW2(W2_.size(), 0.0f);
  std::vector<float> db2(b2_.size(), 0.0f);

  float accum_loss = 0.0f;

  // The whole batch at once. This was a loop over samples doing a matrix-vector
  // product per sample per layer, which is the same arithmetic but gives a
  // device nothing to do: 32 dispatches of 512 numbers each are all overhead.
  // Gathered into matrices, the five products below are big enough to hand to
  // whatever the substrate registered — and identical arithmetic if it did not.
  const int Nb = (int)batch.size();
  const int Din = (int)in_dim, Dh = (int)hidden_dim, Dout = (int)out_dim;

  std::vector<float> X((size_t)Nb * Din, 0.0f);
  for (int n = 0; n < Nb; ++n) {
    const auto& sample = batch[(size_t)n];
    float* x = &X[(size_t)n * Din];
    for (size_t i = 0; i < kSubstrateStateDim && i < sample.s_t.vector.size(); ++i) x[i] = sample.s_t.vector[i];
    for (size_t i = 0; i < kActionVectorDim && i < sample.a_t.size(); ++i) x[kSubstrateStateDim + i] = sample.a_t[i];
  }

  // W1_ is [hidden x in] and W2_ is [out x hidden], so the forward products
  // need them the other way round. Transposing costs one pass over the weights;
  // the products cost N times that.
  std::vector<float> W1T((size_t)Din * Dh), W2T((size_t)Dh * Dout);
  for (int i = 0; i < Dh; ++i) for (int j = 0; j < Din; ++j) W1T[(size_t)j * Dh + i] = W1_[(size_t)i * Din + j];
  for (int i = 0; i < Dout; ++i) for (int j = 0; j < Dh; ++j) W2T[(size_t)j * Dout + i] = W2_[(size_t)i * Dh + j];

  // 1. layer one, pre-activation:  Hpre[N x hidden] = X[N x in] * W1T[in x hidden]
  std::vector<float> Hpre((size_t)Nb * Dh, 0.0f);
  matmul(X.data(), W1T.data(), Hpre.data(), Nb, Din, Dh);
  std::vector<float> H((size_t)Nb * Dh, 0.0f);
  for (int n = 0; n < Nb; ++n) {
    for (int i = 0; i < Dh; ++i) {
      H[(size_t)n * Dh + i] = freestanding_gelu(Hpre[(size_t)n * Dh + i] + b1_[(size_t)i]);
    }
  }

  // 2. layer two, pre-activation:  Ypre[N x out] = H[N x hidden] * W2T[hidden x out]
  std::vector<float> Ypre((size_t)Nb * Dout, 0.0f);
  matmul(H.data(), W2T.data(), Ypre.data(), Nb, Dh, Dout);

  // The loss and dL/dYpre, elementwise, exactly as the per-sample loop had it:
  // y = s_t + 0.1 * gelu(pre), squared error against s_t1, and the 0.1 carried
  // into the gradient.
  std::vector<float> dY((size_t)Nb * Dout, 0.0f);
  for (int n = 0; n < Nb; ++n) {
    const auto& sample = batch[(size_t)n];
    for (int i = 0; i < Dout; ++i) {
      const float pre = Ypre[(size_t)n * Dout + i] + b2_[(size_t)i];
      const float base = ((size_t)i < sample.s_t.vector.size()) ? sample.s_t.vector[(size_t)i] : 0.0f;
      const float y = base + 0.1f * freestanding_gelu(pre);
      const float target = ((size_t)i < sample.s_t1.vector.size()) ? sample.s_t1.vector[(size_t)i] : 0.0f;
      const float diff = y - target;
      accum_loss += diff * diff;
      // dL/dpre = 2/Dout * diff * 0.1 * gelu'(pre)
      const float grad_out = (2.0f / (float)Dout) * diff * 0.1f * freestanding_gelu_grad(pre);
      dY[(size_t)n * Dout + i] = grad_out;
      db2[(size_t)i] += grad_out;
    }
  }

  // 3. dW2[out x hidden] = dY^T[out x N] * H[N x hidden]  — already dW2's layout
  {
    std::vector<float> dYT((size_t)Dout * Nb, 0.0f);
    for (int n = 0; n < Nb; ++n) for (int i = 0; i < Dout; ++i) dYT[(size_t)i * Nb + n] = dY[(size_t)n * Dout + i];
    matmul(dYT.data(), H.data(), dW2.data(), Dout, Nb, Dh);
  }

  // 4. dH[N x hidden] = dY[N x out] * W2_[out x hidden]  — W2_ needs no transpose here
  std::vector<float> dH((size_t)Nb * Dh, 0.0f);
  matmul(dY.data(), W2_.data(), dH.data(), Nb, Dout, Dh);

  // The activation's derivative, taken at the PRE-activation: the exact
  // derivative of the GELU the forward computes. (This was a two-slope guess
  // applied to the post-activation value: 1 where h > 0, 0.05 elsewhere.)
  std::vector<float> dHpre((size_t)Nb * Dh, 0.0f);
  for (int n = 0; n < Nb; ++n) {
    for (int i = 0; i < Dh; ++i) {
      const float dgelu = freestanding_gelu_grad(Hpre[(size_t)n * Dh + i] + b1_[(size_t)i]);
      const float g = dH[(size_t)n * Dh + i] * dgelu;
      dHpre[(size_t)n * Dh + i] = g;
      db1[(size_t)i] += g;
    }
  }

  // 5. dW1[hidden x in] = dHpre^T[hidden x N] * X[N x in]  — already dW1's layout
  {
    std::vector<float> dHpreT((size_t)Dh * Nb, 0.0f);
    for (int n = 0; n < Nb; ++n) for (int i = 0; i < Dh; ++i) dHpreT[(size_t)i * Nb + n] = dHpre[(size_t)n * Dh + i];
    matmul(dHpreT.data(), X.data(), dW1.data(), Dh, Nb, Din);
  }

  uint64_t t = train_steps_.fetch_add(1) + 1;
  float N = static_cast<float>(batch.size());
  float corr1 = 1.0f - std::pow(beta1, static_cast<float>(t));
  float corr2 = 1.0f - std::pow(beta2, static_cast<float>(t));

  // AdamW (Loshchilov & Hutter): the decay is DECOUPLED — applied to the
  // weight directly, not added to the gradient where Adam's normalisation would
  // rescale it. Biases are not decayed.
  for (size_t i = 0; i < W2_.size(); ++i) {
    float g = dW2[i] / N;
    mW2_[i] = beta1 * mW2_[i] + (1.0f - beta1) * g;
    vW2_[i] = beta2 * vW2_[i] + (1.0f - beta2) * g * g;
    float m_hat = mW2_[i] / corr1;
    float v_hat = vW2_[i] / corr2;
    W2_[i] -= lr * (m_hat / (std::sqrt(v_hat) + eps) + weight_decay * W2_[i]);
  }
  for (size_t i = 0; i < b2_.size(); ++i) {
    float g = db2[i] / N;
    mb2_[i] = beta1 * mb2_[i] + (1.0f - beta1) * g;
    vb2_[i] = beta2 * vb2_[i] + (1.0f - beta2) * g * g;
    b2_[i] -= lr * (mb2_[i] / corr1) / (std::sqrt(vb2_[i] / corr2) + eps);
  }

  // AdamW update for W1 (decoupled decay), b1 (no decay)
  for (size_t i = 0; i < W1_.size(); ++i) {
    float g = dW1[i] / N;
    mW1_[i] = beta1 * mW1_[i] + (1.0f - beta1) * g;
    vW1_[i] = beta2 * vW1_[i] + (1.0f - beta2) * g * g;
    float m_hat = mW1_[i] / corr1;
    float v_hat = vW1_[i] / corr2;
    W1_[i] -= lr * (m_hat / (std::sqrt(v_hat) + eps) + weight_decay * W1_[i]);
  }
  for (size_t i = 0; i < b1_.size(); ++i) {
    float g = db1[i] / N;
    mb1_[i] = beta1 * mb1_[i] + (1.0f - beta1) * g;
    vb1_[i] = beta2 * vb1_[i] + (1.0f - beta2) * g * g;
    b1_[i] -= lr * (mb1_[i] / corr1) / (std::sqrt(vb1_[i] / corr2) + eps);
  }

  float avg_loss = accum_loss / (N * static_cast<float>(out_dim));
  last_loss_.store(avg_loss);
  // The step ran on the CPU (and on whatever matmul offload is registered for
  // the five products above). `cuda_backend` is not used: it never was, beyond
  // a busy_lanes() call whose answer was discarded.
  (void)cuda_backend;
  return avg_loss;
}

// Built on first use, not before main: 256K normal draws into 3 MB of weights
// was ~8 ms at the start of EVERY aios process (aios exec, aios-sat, the
// relay...) whether or not it ever asked for a policy. Same seed, same weights.
ActionPolicyHead* get_global_action_policy_head() {
  static ActionPolicyHead policy;
  return &policy;
}

ActionPolicyHead::ActionPolicyHead() {
  size_t in_dim = kSubstrateStateDim; // 1024
  size_t hidden_dim = 256;
  size_t out_dim = kActionVectorDim; // 128

  W1_pol_.resize(hidden_dim * in_dim);
  b1_pol_.resize(hidden_dim, 0.0f);
  W2_pol_.resize(out_dim * hidden_dim);
  b2_pol_.resize(out_dim, 0.0f);

  mW1_pol_.resize(W1_pol_.size(), 0.0f); vW1_pol_.resize(W1_pol_.size(), 0.0f);
  mb1_pol_.resize(b1_pol_.size(), 0.0f); vb1_pol_.resize(b1_pol_.size(), 0.0f);
  mW2_pol_.resize(W2_pol_.size(), 0.0f); vW2_pol_.resize(W2_pol_.size(), 0.0f);
  mb2_pol_.resize(b2_pol_.size(), 0.0f); vb2_pol_.resize(b2_pol_.size(), 0.0f);

  std::mt19937 rng(1337);
  float scale1 = std::sqrt(2.0f / static_cast<float>(in_dim));
  std::normal_distribution<float> dist1(0.0f, scale1);
  for (float& w : W1_pol_) w = dist1(rng);

  float scale2 = std::sqrt(2.0f / static_cast<float>(hidden_dim));
  std::normal_distribution<float> dist2(0.0f, scale2);
  for (float& w : W2_pol_) w = dist2(rng);
}

static float freestanding_tanh(float x) {
  float x_sq = x * x;
  float val = x * (27.0f + x_sq) / (27.0f + 9.0f * x_sq);
  return std::max(-1.0f, std::min(1.0f, val));
}

std::vector<float> ActionPolicyHead::action_mean(const SubstrateState& s_t) const {
  hal::LockGuard lock(mu_);
  size_t in_dim = kSubstrateStateDim;
  size_t hidden_dim = 256;
  size_t out_dim = kActionVectorDim;
  std::vector<float> a_t(out_dim, 0.0f);

  std::vector<float> h(hidden_dim, 0.0f);
  for (size_t i = 0; i < hidden_dim; ++i) {
    float sum = b1_pol_[i];
    const float* row = &W1_pol_[i * in_dim];
    for (size_t j = 0; j < in_dim && j < s_t.vector.size(); ++j) {
      sum += row[j] * s_t.vector[j];
    }
    h[i] = freestanding_gelu(sum);
  }

  for (size_t i = 0; i < out_dim; ++i) {
    float sum = b2_pol_[i];
    const float* row = &W2_pol_[i * hidden_dim];
    for (size_t j = 0; j < hidden_dim; ++j) {
      sum += row[j] * h[j];
    }
    a_t[i] = freestanding_tanh(sum);
  }

  return a_t;
}

// The executed action is a SAMPLE around that mean. This is what makes the
// policy gradient non-degenerate: the trace records an action the network did
// not deterministically choose, so (a_t - mu) carries information the advantage
// can weight up or down.
std::vector<float> ActionPolicyHead::select_action(const SubstrateState& s_t) {
  std::vector<float> a_t = action_mean(s_t);
  static std::mt19937 explore_rng(0x5EED2026u);  // plain static: TLS is not available freestanding
  std::normal_distribution<float> noise(0.0f, explore_sigma_);
  for (float& v : a_t) {
    v += noise(explore_rng);
    if (v < -1.0f) v = -1.0f;
    if (v > 1.0f) v = 1.0f;
  }
  return a_t;
}

float ActionPolicyHead::evaluate_loss(const SubstrateState& s_t, const std::vector<float>& target_a_t) {
  std::vector<float> pred_a = select_action(s_t);
  float mse = 0.0f;
  size_t dim = std::min(pred_a.size(), target_a_t.size());
  for (size_t i = 0; i < dim; ++i) {
    float diff = pred_a[i] - target_a_t[i];
    mse += diff * diff;
  }
  return mse / static_cast<float>(dim > 0 ? dim : 1);
}

Result<float> ActionPolicyHead::train_step(const std::vector<TraceSample>& batch, compute::CudaComputeBackend* cuda_backend) {
  hal::LockGuard lock(mu_);
  if (batch.empty()) return last_loss_.load();

  size_t in_dim = kSubstrateStateDim;
  size_t hidden_dim = 256;
  size_t out_dim = kActionVectorDim;
  float lr = 0.005f;
  float beta1 = 0.9f;
  float beta2 = 0.999f;
  float eps = 1e-8f;
  float accum_loss = 0.0f;

  // --- reward, baseline, advantage -----------------------------------------
  // TraceSample::reward is the MEASURED drop in context switches at equal work
  // (see measured_reward), written by `jepa record` when the transition was
  // observed. The baseline is a running mean of returns; subtracting it is what
  // keeps the gradient from being a constant push in one direction — an action
  // is reinforced only insofar as it did better than what this policy usually
  // gets. Advantages are then scaled by their own spread so the step size does
  // not depend on the units of the instrument.
  float N = static_cast<float>(batch.size());
  float mean_return = 0.0f;
  for (const auto& s : batch) mean_return += s.reward;
  mean_return /= N;

  float baseline = baseline_init_ ? baseline_.load() : mean_return;
  baseline_init_ = true;
  baseline_.store(0.9f * baseline + 0.1f * mean_return);
  last_return_.store(mean_return);

  std::vector<float> adv(batch.size(), 0.0f);
  float adv_var = 0.0f;
  for (size_t i = 0; i < batch.size(); ++i) {
    adv[i] = batch[i].reward - baseline;
    adv_var += adv[i] * adv[i];
  }
  float adv_scale = std::sqrt(adv_var / N) + 1e-6f;
  for (float& v : adv) v /= adv_scale;

  float inv_sigma_sq = 1.0f / (explore_sigma_ * explore_sigma_);

  std::vector<float> dW1(W1_pol_.size(), 0.0f);
  std::vector<float> db1(b1_pol_.size(), 0.0f);
  std::vector<float> dW2(W2_pol_.size(), 0.0f);
  std::vector<float> db2(b2_pol_.size(), 0.0f);

  for (size_t sample_idx = 0; sample_idx < batch.size(); ++sample_idx) {
    const auto& sample = batch[sample_idx];
    float advantage = adv[sample_idx];
    std::vector<float> h(hidden_dim, 0.0f), hpre(hidden_dim, 0.0f);
    for (size_t i = 0; i < hidden_dim; ++i) {
      float sum = b1_pol_[i];
      const float* row = &W1_pol_[i * in_dim];
      for (size_t j = 0; j < in_dim && j < sample.s_t.vector.size(); ++j) {
        sum += row[j] * sample.s_t.vector[j];
      }
      hpre[i] = sum;
      h[i] = freestanding_gelu(sum);
    }

    std::vector<float> pred_a(out_dim, 0.0f);
    std::vector<float> dh(hidden_dim, 0.0f);

    for (size_t i = 0; i < out_dim; ++i) {
      float sum = b2_pol_[i];
      const float* row = &W2_pol_[i * hidden_dim];
      for (size_t j = 0; j < hidden_dim; ++j) {
        sum += row[j] * h[j];
      }
      pred_a[i] = freestanding_tanh(sum);
      const float out_pre = sum;

      // pred_a[i] is mu_i, the mean of the Gaussian the executed action was
      // drawn from; sample.a_t[i] is the action the machine actually ran.
      // Surrogate objective to MINIMISE: -A * log pi(a|s), which for a fixed-
      // sigma Gaussian is A * (a - mu)^2 / (2 sigma^2) up to a constant.
      //   d/dmu = -A * (a - mu) / sigma^2
      // Positive advantage pulls mu TOWARD the action that measured better;
      // negative advantage pushes it away. Nothing here compares the head to
      // its own output, so the objective cannot be driven to zero by fitting.
      float acted = (i < sample.a_t.size()) ? sample.a_t[i] : 0.0f;
      float delta = acted - pred_a[i];
      accum_loss += advantage * delta * delta * 0.5f * inv_sigma_sq;
      // The exact derivative of the rational tanh the forward used.
      float dtanh = rational_tanh_grad(out_pre);
      float grad_out = -(advantage * delta * inv_sigma_sq / static_cast<float>(out_dim)) * dtanh;
      db2[i] += grad_out;

      float* drow2 = &dW2[i * hidden_dim];
      for (size_t j = 0; j < hidden_dim; ++j) {
        drow2[j] += grad_out * h[j];
        dh[j] += grad_out * row[j];
      }
    }

    for (size_t j = 0; j < hidden_dim; ++j) {
      float dgelu = freestanding_gelu_grad(hpre[j]);
      float grad_h = dh[j] * dgelu;
      db1[j] += grad_h;
      float* drow1 = &dW1[j * in_dim];
      for (size_t k = 0; k < in_dim && k < sample.s_t.vector.size(); ++k) {
        drow1[k] += grad_h * sample.s_t.vector[k];
      }
    }
  }

  uint64_t t = train_steps_.fetch_add(1) + 1;
  float corr1 = 1.0f - std::pow(beta1, static_cast<float>(t));
  float corr2 = 1.0f - std::pow(beta2, static_cast<float>(t));

  for (size_t i = 0; i < W2_pol_.size(); ++i) {
    float g = dW2[i] / N;
    mW2_pol_[i] = beta1 * mW2_pol_[i] + (1.0f - beta1) * g;
    vW2_pol_[i] = beta2 * vW2_pol_[i] + (1.0f - beta2) * g * g;
    W2_pol_[i] -= lr * (mW2_pol_[i] / corr1) / (std::sqrt(vW2_pol_[i] / corr2) + eps);
  }
  for (size_t i = 0; i < b2_pol_.size(); ++i) {
    float g = db2[i] / N;
    mb2_pol_[i] = beta1 * mb2_pol_[i] + (1.0f - beta1) * g;
    vb2_pol_[i] = beta2 * vb2_pol_[i] + (1.0f - beta2) * g * g;
    b2_pol_[i] -= lr * (mb2_pol_[i] / corr1) / (std::sqrt(vb2_pol_[i] / corr2) + eps);
  }

  for (size_t i = 0; i < W1_pol_.size(); ++i) {
    float g = dW1[i] / N;
    mW1_pol_[i] = beta1 * mW1_pol_[i] + (1.0f - beta1) * g;
    vW1_pol_[i] = beta2 * vW1_pol_[i] + (1.0f - beta2) * g * g;
    W1_pol_[i] -= lr * (mW1_pol_[i] / corr1) / (std::sqrt(vW1_pol_[i] / corr2) + eps);
  }
  for (size_t i = 0; i < b1_pol_.size(); ++i) {
    float g = db1[i] / N;
    mb1_pol_[i] = beta1 * mb1_pol_[i] + (1.0f - beta1) * g;
    vb1_pol_[i] = beta2 * vb1_pol_[i] + (1.0f - beta2) * g * g;
    b1_pol_[i] -= lr * (mb1_pol_[i] / corr1) / (std::sqrt(vb1_pol_[i] / corr2) + eps);
  }

  // The reported number is the surrogate objective (advantage-weighted negative
  // log-likelihood), not an MSE against the head's own output. It tracks the
  // measured return and can legitimately be negative.
  float avg_loss = accum_loss / (N * static_cast<float>(out_dim));
  last_loss_.store(avg_loss);
  (void)cuda_backend;   // CPU step; see JEPAWorldModel::train_step
  return avg_loss;
}

size_t generate_synthetic_traces(ExecutionTraceLogger& logger, size_t count) {
  std::mt19937 rng(2026);
  std::normal_distribution<float> dist(0.0f, 0.1f);
  std::normal_distribution<float> interrupt_noise(0.0f, 0.03f);
  size_t generated = 0;

  for (size_t i = 0; i < count; ++i) {
    SubstrateState s_t;
    s_t.entity_id = Id128{0x1000 + i, 0x5000 + i};
    s_t.timestamp_ms = 1700000000000ULL + i * 10;
    
    // Telemetry channels 0..4
    float cpu_base = 0.20f + 0.10f * freestanding_sin(i * 0.02f);
    float nic_base = 0.25f + 0.15f * freestanding_cos(i * 0.03f);
    float ctx_base = 0.05f + 0.02f * freestanding_sin(i * 0.05f);
    float mem_base = 0.35f + 0.05f * freestanding_sin(i * 0.01f);
    float gpu_base = 0.80f + 0.10f * freestanding_sin(i * 0.04f);

    s_t.vector[0] = cpu_base;
    s_t.vector[1] = nic_base;
    s_t.vector[2] = ctx_base;
    s_t.vector[3] = mem_base;
    s_t.vector[4] = gpu_base;

    for (size_t k = 5; k < kSubstrateStateDim; ++k) {
      s_t.vector[k] = dist(rng);
    }
    // s[5] is the telemetry's "live" flag (sample_kernel_telemetry sets 1 for a
    // measured read). A synthesized state says it is not one, and that label is
    // what lets a restored trace buffer still tell synthetic from recorded.
    s_t.vector[5] = 0.0f;

    std::vector<float> a_t(kActionVectorDim, 0.0f);
    for (size_t k = 0; k < kActionVectorDim; ++k) {
      a_t[k] = freestanding_tanh(dist(rng));
    }

    // Action Impact Bottleneck Modeling: High action intensity (e.g. heavy compile / burst NIC pre-fetch)
    float action_intensity = std::abs(a_t[0]) + std::abs(a_t[1]);

    SubstrateState s_t1;
    s_t1.entity_id = s_t.entity_id;
    s_t1.timestamp_ms = s_t.timestamp_ms + 10;

    // Telemetry transitions with action impact bottlenecks
    s_t1.vector[0] = std::min(1.0f, s_t.vector[0] + 0.05f * action_intensity + interrupt_noise(rng)); // CPU load
    s_t1.vector[1] = std::max(0.0f, s_t.vector[1] - 0.10f * a_t[1] + interrupt_noise(rng)); // NIC depth drained by pre-fetch action
    s_t1.vector[2] = std::min(1.0f, s_t.vector[2] + 0.02f * action_intensity + interrupt_noise(rng)); // Context switches
    s_t1.vector[3] = std::min(1.0f, s_t.vector[3] + 0.01f * action_intensity + interrupt_noise(rng)); // Memory pressure
    s_t1.vector[4] = std::min(1.0f, s_t.vector[4] + 0.03f * a_t[2] + interrupt_noise(rng)); // GPU load

    for (size_t k = 5; k < kSubstrateStateDim; ++k) {
      // Nonlinear physical state transition: s_{t+1} = 0.9 * s_t + 0.1 * a_t[k % 128] + stochastic interrupt noise
      float a_val = a_t[k % kActionVectorDim];
      s_t1.vector[k] = 0.90f * s_t.vector[k] + 0.10f * a_val + interrupt_noise(rng);
    }
    s_t1.vector[5] = 0.0f;   // synthetic, as above

    // Score simulated transitions with the same function the recorder uses, so
    // the policy gradient has a varying return here too. Simulated, not
    // measured — `jepa synthesize` says so; only `jepa record` produces real
    // rewards from the machine's own counters.
    logger.log_transition(s_t, a_t, s_t1, measured_reward(s_t, s_t1));
    generated++;
  }
  return generated;
}

JEPAMindIntelligence::JEPAMindIntelligence() {
  last_action_.resize(kActionVectorDim, 0.05f);
}

Result<StateID> JEPAMindIntelligence::perceive(Surface& s, const StateObject& context) {
  (void)s;
  SubstrateState current_state = SubstrateState::encode_context(context);
  if (!last_state_.vector.empty()) {
    logger_.log_transition(last_state_, last_action_, current_state, 1.0f);
    get_global_trace_logger()->log_transition(last_state_, last_action_, current_state, 1.0f);
  }
  last_state_ = current_state;
  last_action_ = policy_.select_action(current_state);
  return context.id;
}

Result<GraphID> JEPAMindIntelligence::think(Surface& s, const StateObject& perceived) {
  (void)s;
  SubstrateState predicted = model_.predict_next_state(last_state_, last_action_);
  update_shared_state_region(last_state_, last_action_, predicted);
  GraphID gid;
  gid.hi = perceived.id.hi ^ 0xFEEDFACEull;
  gid.lo = perceived.id.lo ^ 0xC0FFEEull;
  return gid;
}

Result<StateID> JEPAMindIntelligence::act(Surface& s, const StateObject& perceived,
                                          const ComputeGraph* graph, const ExecutionReport* report) {
  (void)s;
  (void)graph;
  (void)report;
  return perceived.id;
}

std::unique_ptr<Intelligence> make_jepa_world_model_intelligence() {
  return std::make_unique<JEPAMindIntelligence>();
}

LearnStep learn_once(size_t batch_size) {
  LearnStep out;
  if (!telemetry_is_live()) {
    out.refused = "no substrate telemetry provider — every state would be the synthetic driver";
    return out;
  }
  if (!settle_is_live()) {
    out.refused = "no substrate settler — the two reads would bracket no work and every delta would be 0";
    return out;
  }

  auto* model = get_global_jepa_world_model();
  auto* policy = get_global_action_policy_head();
  auto* logger = get_global_trace_logger();

  // s_t, then an action drawn from the policy's Gaussian — sampled, not the
  // mean, because REINFORCE has nothing to reweight if the policy only ever
  // sees the action it already preferred.
  const SubstrateState s_t = SubstrateState::sample_kernel_telemetry();
  const std::vector<float> a_t = policy->select_action(s_t);

  SubstrateActuation applied;
  (void)apply_action(a_t, &applied);
  out.slice_applied = applied.scheduler_slice_ticks;

  // The measurement window. Everything between these two reads is the FIXED
  // workload the settler runs — deliberately, so the reward reflects the action
  // and not how busy the machine happened to be. The gradient step below is
  // outside it, which is what keeps the learner's own cost from being scored as
  // though the policy had caused it.
  (void)substrate_settle();
  const SubstrateState s_t1 = SubstrateState::sample_kernel_telemetry();

  out.reward = measured_reward(s_t, s_t1);
  logger->log_transition(s_t, a_t, s_t1, out.reward);
  update_shared_state_region(s_t, a_t, model->predict_next_state(s_t, a_t));

  auto batch = logger->get_samples(batch_size);
  auto rw = model->train_step(batch);
  if (rw.ok()) out.world_loss = *rw;
  auto rp = policy->train_step(batch);
  if (rp.ok()) out.policy_loss = *rp;

  out.ran = true;
  return out;
}

// ---------------------------------------------------------------------------
// Checkpoints.
// ---------------------------------------------------------------------------
// A checkpoint is self-describing on purpose. Two model files already existed
// in this tree with no reader and no writer anywhere in the source, and their
// layout could not be recovered from the bytes: the candidate offsets were
// statistically indistinguishable, so any decode would have been a guess that
// silently produced weights. Hence the magic and the shape list — a buffer that
// does not say what it is gets refused, not interpreted.
namespace {

constexpr uint32_t kCheckpointMagic = 0x314A5741;   // "AWJ1", little-endian
constexpr uint32_t kCheckpointVersion = 1;

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}
void put_u64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}
void put_f32(std::vector<uint8_t>& out, float f) {
  uint32_t bits = 0;
  static_assert(sizeof(float) == sizeof(uint32_t), "f32 punning");
  std::memcpy(&bits, &f, sizeof(bits));
  put_u32(out, bits);
}
void put_tensor(std::vector<uint8_t>& out, const std::vector<float>& t) {
  put_u64(out, t.size());
  for (float f : t) put_f32(out, f);
}

// A cursor that can run off the end without reading past it. Every get_ below
// checks before it reads and latches `bad` — a truncated checkpoint must be a
// refusal, not a read of whatever follows the buffer.
struct Reader {
  const std::vector<uint8_t>& b;
  size_t at = 0;
  bool bad = false;
  bool want(size_t n) {
    if (bad || at + n > b.size()) { bad = true; return false; }
    return true;
  }
  uint32_t u32() {
    if (!want(4)) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(b[at + static_cast<size_t>(i)]) << (i * 8);
    at += 4;
    return v;
  }
  uint64_t u64() {
    if (!want(8)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(b[at + static_cast<size_t>(i)]) << (i * 8);
    at += 8;
    return v;
  }
  float f32() {
    uint32_t bits = u32();
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
  }
  // Reads into `t` only if the stored length is exactly what this model has.
  // A shape mismatch is the signal that this checkpoint belongs to a different
  // architecture, and resizing to fit it would load a stranger's model.
  bool tensor(std::vector<float>& t) {
    uint64_t n = u64();
    if (bad || n != t.size()) { bad = true; return false; }
    for (size_t i = 0; i < t.size(); ++i) t[i] = f32();
    return !bad;
  }
};

// Header shared by both checkpoint kinds. `kind` separates them so a policy
// file handed to the predictor is refused by name rather than by shape.
void put_header(std::vector<uint8_t>& out, uint32_t kind, uint64_t steps) {
  put_u32(out, kCheckpointMagic);
  put_u32(out, kCheckpointVersion);
  put_u32(out, kind);
  put_u32(out, 0);   // reserved, keeps the float payload 8-byte aligned
  put_u64(out, steps);
}

bool check_header(Reader& r, uint32_t want_kind, uint64_t& steps, std::string* why) {
  const uint32_t magic = r.u32();
  if (r.bad || magic != kCheckpointMagic) {
    if (why) *why = "not an aios model checkpoint (bad magic)";
    return false;
  }
  const uint32_t version = r.u32();
  if (version != kCheckpointVersion) {
    if (why) *why = "checkpoint version " + std::to_string(version) + ", this build writes " + std::to_string(kCheckpointVersion);
    return false;
  }
  const uint32_t kind = r.u32();
  if (kind != want_kind) {
    if (why) *why = std::string("checkpoint is the ") + (kind == 1 ? "predictor" : kind == 2 ? "policy head" : "unknown") + ", not the one being loaded";
    return false;
  }
  (void)r.u32();   // reserved
  steps = r.u64();
  return !r.bad;
}

constexpr uint32_t kKindPredictor = 1;
constexpr uint32_t kKindPolicy = 2;
constexpr uint32_t kKindTraces = 3;

}  // namespace

std::vector<uint8_t> ExecutionTraceLogger::serialize() const {
  hal::LockGuard lock(mu_);
  std::vector<uint8_t> out;
  // Header's step field carries the capacity, so a buffer written by a machine
  // with a different ring size is still readable — the count is what matters.
  put_header(out, kKindTraces, capacity_);
  put_u64(out, buffer_.size());
  for (const auto& s : buffer_) {
    put_u64(out, s.s_t.timestamp_ms);
    put_tensor(out, s.s_t.vector);
    put_tensor(out, s.a_t);
    put_u64(out, s.s_t1.timestamp_ms);
    put_tensor(out, s.s_t1.vector);
    put_f32(out, s.reward);
  }
  return out;
}

bool ExecutionTraceLogger::deserialize(const std::vector<uint8_t>& blob, std::string* why) {
  hal::LockGuard lock(mu_);
  Reader r{blob};
  uint64_t cap = 0;
  if (!check_header(r, kKindTraces, cap, why)) return false;
  const uint64_t n = r.u64();
  if (r.bad || n > capacity_) {
    if (why) *why = "trace file holds " + std::to_string(n) + " transition(s), more than this buffer's "
                  + std::to_string(capacity_);
    return false;
  }
  // Built aside, as the models are: a truncated trace file must leave the
  // machine with the experience it already had, not half of someone else's.
  std::vector<TraceSample> loaded;
  loaded.reserve(static_cast<size_t>(n));
  for (uint64_t i = 0; i < n; ++i) {
    TraceSample s;
    s.s_t.timestamp_ms = r.u64();
    s.a_t.assign(kActionVectorDim, 0.0f);
    if (!r.tensor(s.s_t.vector) || !r.tensor(s.a_t)) {
      if (why) *why = "trace file is truncated or its vectors are not this machine's shape";
      return false;
    }
    s.s_t1.timestamp_ms = r.u64();
    if (!r.tensor(s.s_t1.vector)) {
      if (why) *why = "trace file is truncated or its vectors are not this machine's shape";
      return false;
    }
    s.reward = r.f32();
    if (r.bad) {
      if (why) *why = "trace file is truncated";
      return false;
    }
    loaded.push_back(std::move(s));
  }
  buffer_ = std::move(loaded);
  return true;
}

std::vector<uint8_t> JEPAWorldModel::serialize() const {
  hal::LockGuard lock(mu_);
  std::vector<uint8_t> out;
  out.reserve(16 + (W1_.size() + b1_.size() + W2_.size() + b2_.size()) * 3 * 4 + 12 * 8);
  put_header(out, kKindPredictor, train_steps_.load());
  put_f32(out, last_loss_.load());
  put_f32(out, std_loss_.load());
  for (const auto* t : {&W1_, &b1_, &W2_, &b2_,
                        &mW1_, &mb1_, &mW2_, &mb2_,
                        &vW1_, &vb1_, &vW2_, &vb2_}) {
    put_tensor(out, *t);
  }
  return out;
}

bool JEPAWorldModel::deserialize(const std::vector<uint8_t>& blob, std::string* why) {
  hal::LockGuard lock(mu_);
  Reader r{blob};
  uint64_t steps = 0;
  if (!check_header(r, kKindPredictor, steps, why)) return false;
  const float loss = r.f32();
  const float stdl = r.f32();

  // Decoded into copies first. A checkpoint that turns out to be truncated
  // halfway through mW2_ must leave a MODEL BEHIND, not a half-loaded one —
  // this is the difference between a failed load and a corrupted process.
  std::vector<float> W1 = W1_, b1 = b1_, W2 = W2_, b2 = b2_;
  std::vector<float> mW1 = mW1_, mb1 = mb1_, mW2 = mW2_, mb2 = mb2_;
  std::vector<float> vW1 = vW1_, vb1 = vb1_, vW2 = vW2_, vb2 = vb2_;
  for (auto* t : {&W1, &b1, &W2, &b2, &mW1, &mb1, &mW2, &mb2, &vW1, &vb1, &vW2, &vb2}) {
    if (!r.tensor(*t)) {
      if (why) *why = "checkpoint is truncated or its tensor shapes are not this predictor's";
      return false;
    }
  }

  W1_ = std::move(W1); b1_ = std::move(b1); W2_ = std::move(W2); b2_ = std::move(b2);
  mW1_ = std::move(mW1); mb1_ = std::move(mb1); mW2_ = std::move(mW2); mb2_ = std::move(mb2);
  vW1_ = std::move(vW1); vb1_ = std::move(vb1); vW2_ = std::move(vW2); vb2_ = std::move(vb2);
  train_steps_.store(steps);
  last_loss_.store(loss);
  std_loss_.store(stdl);
  return true;
}

std::vector<uint8_t> ActionPolicyHead::serialize() const {
  hal::LockGuard lock(mu_);
  std::vector<uint8_t> out;
  out.reserve(16 + (W1_pol_.size() + b1_pol_.size() + W2_pol_.size() + b2_pol_.size()) * 3 * 4 + 12 * 8);
  put_header(out, kKindPolicy, train_steps_.load());
  put_f32(out, last_loss_.load());
  put_f32(out, last_return_.load());
  put_f32(out, baseline_.load());
  put_f32(out, explore_sigma_);
  put_u32(out, baseline_init_ ? 1u : 0u);
  put_u32(out, 0);   // reserved, realigns the float payload
  for (const auto* t : {&W1_pol_, &b1_pol_, &W2_pol_, &b2_pol_,
                        &mW1_pol_, &mb1_pol_, &mW2_pol_, &mb2_pol_,
                        &vW1_pol_, &vb1_pol_, &vW2_pol_, &vb2_pol_}) {
    put_tensor(out, *t);
  }
  return out;
}

bool ActionPolicyHead::deserialize(const std::vector<uint8_t>& blob, std::string* why) {
  hal::LockGuard lock(mu_);
  Reader r{blob};
  uint64_t steps = 0;
  if (!check_header(r, kKindPolicy, steps, why)) return false;
  const float loss = r.f32();
  const float ret = r.f32();
  const float base = r.f32();
  const float sigma = r.f32();
  const bool base_init = r.u32() != 0;
  (void)r.u32();

  std::vector<float> W1 = W1_pol_, b1 = b1_pol_, W2 = W2_pol_, b2 = b2_pol_;
  std::vector<float> mW1 = mW1_pol_, mb1 = mb1_pol_, mW2 = mW2_pol_, mb2 = mb2_pol_;
  std::vector<float> vW1 = vW1_pol_, vb1 = vb1_pol_, vW2 = vW2_pol_, vb2 = vb2_pol_;
  for (auto* t : {&W1, &b1, &W2, &b2, &mW1, &mb1, &mW2, &mb2, &vW1, &vb1, &vW2, &vb2}) {
    if (!r.tensor(*t)) {
      if (why) *why = "checkpoint is truncated or its tensor shapes are not this policy head's";
      return false;
    }
  }

  W1_pol_ = std::move(W1); b1_pol_ = std::move(b1); W2_pol_ = std::move(W2); b2_pol_ = std::move(b2);
  mW1_pol_ = std::move(mW1); mb1_pol_ = std::move(mb1); mW2_pol_ = std::move(mW2); mb2_pol_ = std::move(mb2);
  vW1_pol_ = std::move(vW1); vb1_pol_ = std::move(vb1); vW2_pol_ = std::move(vW2); vb2_pol_ = std::move(vb2);
  train_steps_.store(steps);
  last_loss_.store(loss);
  last_return_.store(ret);
  baseline_.store(base);
  explore_sigma_ = sigma;
  baseline_init_ = base_init;
  return true;
}

}  // namespace aios::intelligence

