#pragma once
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <atomic>
#include <cmath>
#include "aios/core/ids.hpp"
#include "aios/core/result.hpp"
#include "aios/hal/sync.hpp"
#include "aios/intelligence/intelligence.hpp"
#include "aios/compute/backends.hpp"

namespace aios::intelligence {

constexpr size_t kSubstrateStateDim = 1024;
constexpr size_t kActionVectorDim = 128;

// 0. The substrate's own instruments and levers.
//
// The world model lives in the kernel core, which knows nothing about the
// machine underneath it. So the substrate (metal/, or the hosted HAL) REGISTERS
// what it can measure and what it can be told to do; the core only calls what
// it was handed. Nothing registered means the model is reading a simulation,
// and telemetry_is_live() says so rather than pretending.
struct SubstrateTelemetry {
  float cpu_load{0.0f};          // fraction of recent ticks that ran a task
  float nic_ring_depth{0.0f};    // queued rx descriptors / ring capacity
  float ctx_switch_rate{0.0f};   // context switches per tick
  float mem_pressure{0.0f};      // heap bytes used / heap bytes total
  float compute_occupancy{0.0f}; // busy compute lanes / lanes present
  // Frames the interface dropped or errored over frames it received. This is
  // the instrument the network lever is judged by: a refill window is only
  // right or wrong relative to what the machine is losing.
  float packet_loss{0.0f};
  // Run-queue length over processors, so "correct the load" has a number.
  float load_imbalance{0.0f};
  // The switch rate BEFORE normalisation, so the ceiling the normalised figure
  // is divided by can be checked against what the machine actually does instead
  // of taken on trust. A saturated instrument is invisible in the normalised
  // number and obvious in this one.
  float ctx_switch_raw{0.0f};
  // The two numbers the rate is a ratio OF, so a rate that looks wrong can be
  // taken apart instead of argued about. A ratio of 1.0 can mean "one switch per
  // tick over a long interval" or "three switches over three ticks because the
  // interval was almost nothing", and those are different bugs.
  uint64_t switch_events{0};
  uint64_t interval_ticks{0};
  // The slice the machine is ACTUALLY holding, read back from the substrate.
  // Every diagnostic so far printed the slice the policy asked for, which is
  // decode_action's output and says nothing about whether the sink applied it.
  // Asked-versus-held is the difference between "the lever does nothing" and
  // "the lever was never pulled".
  uint32_t scheduler_slice_now{0};
  uint64_t timestamp_ms{0};
  bool live{false};              // false = nobody is measuring; values are synthetic
};

// What the policy head is allowed to change about the machine. Four levers now,
// from a_t[0..3]; the other 124 dimensions are still latent capacity nothing
// reads, and that is still said out loud in `jepa status`.
struct SubstrateActuation {
  uint32_t scheduler_slice_ticks{1};  // a_t[0]: timer ticks a task keeps the CPU
  bool nic_prefetch_enabled{false};   // a_t[1]: sign of the refill window below
  // a_t[1]: how many receive buffers the driver keeps posted. The bool above is
  // the same lever asked as a question; this is the number the driver uses.
  uint32_t nic_refill_window{4};
  // a_t[2]: what the machine puts on its own display. 0 tide, 1 lattice,
  // 2 core, 3 pyramid. A lever a person can SEE, which is the only kind whose
  // effect nobody has to take on trust.
  uint32_t display_scene{0};
  // a_t[3]: the share of measured device time the scheduler will charge an
  // entity before it makes it wait, as a percentage. The lever that answers
  // "optimise a process and correct the load".
  uint32_t compute_quota_pct{100};
};

using TelemetryProvider = SubstrateTelemetry (*)();
using ActuationSink = bool (*)(const SubstrateActuation&);
// "Let this machine actually run a representative slice of work, now." Between
// applying an action and measuring what followed there has to be work, or the
// two telemetry reads are taken microseconds apart on an idle machine and every
// delta is 0 — which is exactly the reward signal the recorder was getting. The
// substrate owns work (the core cannot spawn a process), so it registers this.
using SubstrateSettle = void (*)();
// How hard the settler should press: 0 idle, 3 thrashing. The core does not know
// what pressure means on any given substrate — that is the substrate's business
// — but the recorder has to be able to SWEEP it, or every transition it records
// is the same transition and the state space is never visited.
using SubstratePressure = void (*)(int level);

void set_telemetry_provider(TelemetryProvider p);
void set_actuation_sink(ActuationSink s);
void set_substrate_settle(SubstrateSettle s);
void set_substrate_pressure_hook(SubstratePressure p);
// Ask for a pressure. Silently does nothing when the substrate registered no
// hook, which is honest: a machine with one workload has one workload.
void set_settle_pressure(int level);
bool pressure_hook_is_live();
bool telemetry_is_live();
bool actuation_is_live();
bool settle_is_live();
// Runs the registered settler; returns false if there is none, in which case the
// caller is measuring an idle machine and should say so rather than report 0.
bool substrate_settle();
SubstrateTelemetry read_substrate_telemetry();
// The last reading, without taking another: every provider differences against
// its own previous call, so reading again restarts the interval and answers 0.
SubstrateTelemetry last_substrate_telemetry();

// Decode the policy head's raw action vector into levers the substrate has.
SubstrateActuation decode_action(const std::vector<float>& a_t);
// Push it to the substrate. Returns false if no sink is registered.
bool apply_action(const std::vector<float>& a_t, SubstrateActuation* applied = nullptr);

// 1. SubstrateState: Continuous latent vector representation (s_t in R^1024)
struct SubstrateState {
  Id128 entity_id;
  uint64_t timestamp_ms{0};
  std::vector<float> vector; // 1024-dimensional latent state vector

  SubstrateState();
  static SubstrateState encode_context(const StateObject& context);
  static SubstrateState sample_kernel_telemetry();
};

// The reward the policy head is trained against: the negated context-switch cost
// of the settled interval — the kernel's own switch counter (dim 2) over the work
// that actually ran (dim 0) — with the workload held fixed by the registered
// settler. Same instrument and same axis `host jepa prove` reports going 460 -> 30,
// scored one transition at a time. Not a status string.
float measured_reward(const SubstrateState& s_t, const SubstrateState& s_t1);

// ---- where the predictor's matrix products may run ------------------------
// The core cannot know what accelerators this machine has — that is the whole
// point of the host boundary — so it does not reach for one. Whoever owns the
// compute fabric REGISTERS a matrix product here, and the predictor uses it for
// the two big ones in its forward pass and the two in its backward pass. With
// nothing registered every loop below is the scalar C++ it always was.
//
// C[M x N] = A[M x K] * B[K x N], all row-major f32. Returning false means the
// offload declined this one and the caller must do it itself — a device that
// silently returned nothing would be worse than no device.
using MatmulOffload = bool (*)(const float* A, const float* B, float* C, int M, int K, int N);
void set_matmul_offload(MatmulOffload fn, const char* what);
bool matmul_offload_is_live();
const char* matmul_offload_name();
// What it has actually done, so the claim can be checked rather than believed.
struct MatmulOffloadCount { uint64_t offloaded = 0, declined = 0, macs = 0; };
MatmulOffloadCount matmul_offload_counters();
// C = A*B through the registered device when there is one, on the CPU when
// there is not. Both paths are here so there is one place the choice is made.
void matmul(const float* A, const float* B, float* C, int M, int K, int N);

// 2. Trace sample (s_t, a_t, s_{t+1}) for JEPA latent world model training
struct TraceSample {
  SubstrateState s_t;
  std::vector<float> a_t; // 128-dimensional action vector
  SubstrateState s_t1;
  float reward{1.0f};
};

// 3. ExecutionTraceLogger: Ring buffer / storage logging transitions for background continuous training
class ExecutionTraceLogger {
 public:
  ExecutionTraceLogger(size_t capacity = 1000);
  void log_transition(const SubstrateState& s_t, const std::vector<float>& a_t, const SubstrateState& s_t1, float reward = 1.0f);
  // A UNIFORM DRAW from everything recorded, not the most recent batch_size.
  // Returning the tail made every training step in a run see byte-identical
  // input: 50 steps and 500 steps left the loss at 0.00107596 and 0.0010759,
  // which is one batch memorised rather than a substrate learned. A learner
  // that keeps running needs each step to see a different draw of what the
  // machine has actually done.
  std::vector<TraceSample> get_samples(size_t batch_size) const;
  // The most recent `count`, for a caller that specifically wants recency —
  // "what has this machine just been doing" is a different question from
  // "what has it done", and the recorder's own reporting wants the former.
  std::vector<TraceSample> get_recent(size_t count) const;
  size_t size() const;
  // How many of them were MEASURED (the state's live flag, s[5], set by the
  // telemetry read); the rest were synthesized. Survives a checkpoint, because
  // the flag is part of the state vector that is serialized.
  size_t measured_count() const;
  void clear();

  // What this machine has SEEN, as bytes — the same split as the models.
  // Persisting weights without the transitions that produced them means every
  // restart trains a well-practised model against a cold buffer: driven across
  // four processes the prediction loss went 0.020, 0.007, 0.015, 0.045 while
  // the step count rose 60 -> 240, because each run had only its own 60
  // samples to draw from. Experience is the slower thing to re-earn of the two.
  std::vector<uint8_t> serialize() const;
  bool deserialize(const std::vector<uint8_t>& blob, std::string* why = nullptr);

 private:
  size_t capacity_;
  mutable hal::Mutex mu_;
  std::vector<TraceSample> buffer_;
  // xorshift64*, not <random>: this runs inside the kernel core, which is
  // built freestanding for bare metal. Seeded to a fixed non-zero constant so
  // a run is reproducible; advanced under mu_ so parallel draws differ.
  mutable uint64_t draw_state_{0x9E3779B97F4A7C15ull};
};

// 4. JEPAWorldModel: a residual predictor s_hat = s + 0.1 * GELU(W2 GELU(W1 [s;a] + b1) + b2),
// trained on the CPU (its five matrix products go to a registered matmul
// offload when one takes them — `jepa gpu`, `jepa bench`).
class JEPAWorldModel {
 public:
  JEPAWorldModel();
  ~JEPAWorldModel() = default;

  // Forward prediction s_hat_{t+1} = P_phi(s_t, a_t)
  SubstrateState predict_next_state(const SubstrateState& s_t, const std::vector<float>& a_t);

  // Multi-step horizon trajectory prediction s_{t+1..t+K}
  std::vector<SubstrateState> predict_trajectory(const SubstrateState& s_0, const std::vector<std::vector<float>>& actions);

  // Prediction MSE plus 0.1 x a VICReg-style VARIANCE hinge, max(0, 1 - std(s_hat)),
  // over one prediction's components. There is no covariance term, and
  // train_step does not optimise this: it minimises the MSE alone.
  float evaluate_loss(const SubstrateState& s_t, const std::vector<float>& a_t, const SubstrateState& s_t1);

  // One AdamW step (decoupled weight decay) on the prediction MSE, on the CPU.
  // `cuda_backend` is accepted for API compatibility and not used.
  Result<float> train_step(const std::vector<TraceSample>& batch, compute::CudaComputeBackend* cuda_backend = nullptr);

  // Model statistics
  uint64_t total_steps() const { return train_steps_.load(); }
  float current_loss() const { return last_loss_.load(); }
  float variance_loss() const { return std_loss_.load(); }

  // What this model has learned, as bytes. The core does not touch files — it
  // does not know what storage this machine has — so persistence is split: the
  // model produces a buffer, and whoever owns a Host writes it. The AdamW
  // moments travel WITH the weights: restoring parameters without the moments
  // restarts Adam's bias correction from zero and the first steps after a
  // reload move far further than the ones before it, which reads as a model
  // that got worse by being saved.
  std::vector<uint8_t> serialize() const;
  // Replaces the model in place. Refuses a buffer whose magic, version or
  // tensor shapes are not this model's, and leaves the weights untouched when
  // it refuses — a checkpoint from a different architecture loaded silently is
  // indistinguishable from one that never loaded.
  bool deserialize(const std::vector<uint8_t>& blob, std::string* why = nullptr);

 private:
  // 2-layer Residual JEPA Latent Predictor: input 1152 -> hidden 512 -> output 1024 + skip(s_t)
  std::vector<float> W1_; // [512 x (1024 + 128)]
  std::vector<float> b1_; // [512]
  std::vector<float> W2_; // [1024 x 512]
  std::vector<float> b2_; // [1024]

  // AdamW optimizer moments for predictor
  std::vector<float> mW1_, vW1_, mb1_, vb1_;
  std::vector<float> mW2_, vW2_, mb2_, vb2_;

  mutable hal::Mutex mu_;
  std::atomic<uint64_t> train_steps_{0};
  // 0 until a step has run: there is no loss before there is training (these
  // started at 0.008 / 0.001, which `jepa status` printed at 0 steps).
  std::atomic<float> last_loss_{0.0f};
  std::atomic<float> std_loss_{0.0f};
};

// 5. ActionPolicyHead: 2-layer Policy network mapping SubstrateState s_t to Action Vector a_t (R^128)
class ActionPolicyHead {
 public:
  ActionPolicyHead();
  ~ActionPolicyHead() = default;

  // Stochastic policy: the network output is the MEAN of a Gaussian and the
  // returned action is a sample from it. A deterministic head cannot be trained
  // by policy gradient — with a == mu the log-likelihood gradient is identically
  // zero, which is what made the old self-MSE loop print 0 forever.
  std::vector<float> select_action(const SubstrateState& s_t);
  std::vector<float> action_mean(const SubstrateState& s_t) const;
  float evaluate_loss(const SubstrateState& s_t, const std::vector<float>& target_a_t);
  // REINFORCE with an explicit baseline: ascend advantage * grad log pi(a_t|s_t),
  // where the advantage comes from TraceSample::reward (measured, see
  // measured_reward) minus a running baseline.
  Result<float> train_step(const std::vector<TraceSample>& batch, compute::CudaComputeBackend* cuda_backend = nullptr);

  uint64_t total_steps() const { return train_steps_.load(); }
  float current_loss() const { return last_loss_.load(); }        // advantage-weighted policy loss
  float current_return() const { return last_return_.load(); }    // mean measured reward of last batch
  float current_baseline() const { return baseline_.load(); }
  float exploration_sigma() const { return explore_sigma_; }

  // Same split as the predictor: bytes here, files elsewhere. The running
  // baseline is part of the state — a restored policy whose baseline is back
  // at zero scores its first batch as though every measured return were pure
  // advantage, and takes one large wrong step for it.
  std::vector<uint8_t> serialize() const;
  bool deserialize(const std::vector<uint8_t>& blob, std::string* why = nullptr);

 private:
  // 2-layer MLP Policy Head: input 1024 -> hidden 256 -> output 128
  std::vector<float> W1_pol_; // [256 x 1024]
  std::vector<float> b1_pol_; // [256]
  std::vector<float> W2_pol_; // [128 x 256]
  std::vector<float> b2_pol_; // [128]

  // Adam moments for the policy head (plain Adam: no weight decay)
  std::vector<float> mW1_pol_, vW1_pol_, mb1_pol_, vb1_pol_;
  std::vector<float> mW2_pol_, vW2_pol_, mb2_pol_, vb2_pol_;

  // Gaussian exploration width around the network's mean action. Without it the
  // policy never sees an action it did not already prefer, and there is nothing
  // for the advantage to reweight.
  float explore_sigma_{0.15f};

  mutable hal::Mutex mu_;
  std::atomic<uint64_t> train_steps_{0};
  std::atomic<float> last_loss_{0.0f};   // 0 until a step has run
  std::atomic<float> last_return_{0.0f};
  std::atomic<float> baseline_{0.0f};
  bool baseline_init_{false};
};

// 6. JEPAMindIntelligence: Hot-swappable aios::Intelligence implementation
class JEPAMindIntelligence final : public Intelligence {
 public:
  JEPAMindIntelligence();
  ~JEPAMindIntelligence() override = default;

  std::string name() const override { return "jepa-world-model"; }   // CPU-trained; no CUDA path
  Result<StateID> perceive(Surface& s, const StateObject& context) override;
  Result<GraphID> think(Surface& s, const StateObject& perceived) override;
  Result<StateID> act(Surface& s, const StateObject& perceived, const ComputeGraph* graph,
                      const ExecutionReport* report) override;

  JEPAWorldModel& world_model() { return model_; }
  ActionPolicyHead& policy_head() { return policy_; }
  ExecutionTraceLogger& trace_logger() { return logger_; }

 private:
  JEPAWorldModel model_;
  ActionPolicyHead policy_;
  ExecutionTraceLogger logger_;
  SubstrateState last_state_;
  std::vector<float> last_action_;
};

// 7. SharedStateVectorRegion: Shared memory state vector region mapped between kernel and userland
struct SharedStateVectorRegion {
  std::atomic<uint64_t> sequence{0};
  uint64_t timestamp_ms{0};
  float state_vector[kSubstrateStateDim];
  float action_vector[kActionVectorDim];
  float predicted_next_state[kSubstrateStateDim];
};

// 8. One complete turn of the learning loop, against the real machine.
//
// This is what "trains itself" means concretely: read the substrate, choose an
// action, APPLY it, let a fixed workload run, read the substrate again, score
// what followed, record the transition, and take one gradient step on a fresh
// draw of everything recorded so far. It lives in the core rather than in one
// substrate's idle handler so bare metal and a hosted kernel run the identical
// loop instead of two that drift apart.
//
// It refuses rather than pretending when the machine cannot support it: with no
// telemetry provider every state is the synthetic sine driver, and with no
// settler the two reads bracket no work and every delta is 0. Training on
// either is training on nothing, and doing it quietly is worse than not doing
// it at all.
struct LearnStep {
  bool ran = false;
  const char* refused = nullptr;   // why not, when ran == false
  float reward = 0.0f;             // measured_reward of THIS transition
  float world_loss = 0.0f;
  float policy_loss = 0.0f;
  uint32_t slice_applied = 0;      // what the action actually set, read back
};
LearnStep learn_once(size_t batch_size = 64);

std::unique_ptr<Intelligence> make_jepa_world_model_intelligence();
JEPAWorldModel* get_global_jepa_world_model();
ActionPolicyHead* get_global_action_policy_head();
ExecutionTraceLogger* get_global_trace_logger();
SharedStateVectorRegion* get_global_shared_state_region();
void update_shared_state_region(const SubstrateState& s_t, const std::vector<float>& a_t, const SubstrateState& s_t1);
size_t generate_synthetic_traces(ExecutionTraceLogger& logger, size_t count = 500);

}  // namespace aios::intelligence

