#pragma once
// The machine's own language model: a byte-level BPE tokenizer and a small
// transformer decoder, both built here in scalar C++ with no external
// dependency and no host-only header, so this links freestanding into the
// bare-metal image exactly as the world model does.
//
// This is deliberately NOT a wrapper around somebody's inference server. The
// vocabulary is learned by this machine from a corpus this machine writes about
// itself; the weights are trained here by gradient descent; both are persisted
// through a store the substrate registers (a file on a hosted box, nothing at
// all on metal, where it retrains at boot). What that buys is small and it is
// stated plainly in `mind status`: it maps a request onto one of the console
// commands it was trained on, and says a sentence about the current state
// vector. It is not a general assistant and cannot be mistaken for one.
#include <cstdint>
#include <string>
#include <map>
#include <vector>
#include "aios/core/result.hpp"
#include "aios/core/ids.hpp"

namespace aios {
class Scheduler;
class MemoryFabric;
class IdGenerator;
}

namespace aios::intelligence {

// ---- persistence -----------------------------------------------------------
// The core has no filesystem. Whoever owns storage registers these; with no
// store registered the model simply trains on first use and is gone at exit.
using LmStoreRead = bool (*)(const char* name, std::string& out);
using LmStoreWrite = bool (*)(const char* name, const std::string& blob);
void set_lm_store(LmStoreRead r, LmStoreWrite w);
bool lm_store_is_live();

// ---- tokenizer -------------------------------------------------------------
// Byte-level BPE: every one of the 256 bytes is a token, and merges are learned
// from the corpus by repeatedly joining the most frequent adjacent pair. No
// unknown token can exist, because any input decomposes into bytes.
class ByteBpeTokenizer {
 public:
  void train(const std::vector<std::string>& corpus, size_t target_vocab);
  std::vector<int> encode(const std::string& text) const;
  std::string decode(const std::vector<int>& ids) const;
  size_t vocab_size() const { return vocab_.size(); }
  size_t merge_count() const { return merges_.size(); }
  bool trained() const { return !merges_.empty(); }

  void serialize(std::string& out) const;
  bool deserialize(const std::string& blob, size_t& off);

 private:
  struct Merge { int a; int b; int id; };
  std::vector<Merge> merges_;
  std::vector<std::string> vocab_;   // token id -> the bytes it stands for
  int rank_of(int a, int b) const;
};

// ---- transformer decoder ---------------------------------------------------
struct TinyLmConfig {
  int d_model = 64;
  int n_layer = 2;
  int n_head = 4;
  int d_ff = 128;
  int block = 64;      // context length
};

// A pre-LN causal decoder. Forward and backward are both written out here in
// scalar C++ — there is no BLAS, no autograd and no GPU under this.
class TinyTransformerLM {
 public:
  void init(int vocab, const TinyLmConfig& cfg, uint32_t seed = 20260907u);
  bool ready() const { return vocab_ > 0 && !tok_emb_.empty(); }
  const TinyLmConfig& config() const { return cfg_; }
  int vocab() const { return vocab_; }
  uint64_t train_steps() const { return steps_; }
  float last_loss() const { return last_loss_; }
  size_t parameter_count() const;

  // Mean cross-entropy of the sequence under the model (next-token prediction).
  float sequence_loss(const std::vector<int>& ids) const;
  // One AdamW step over a batch of sequences. Returns the mean loss.
  float train_step(const std::vector<std::vector<int>>& batch, float lr);
  // The same step as a ComputeGraph the scheduler places over the fabric —
  // forward, backward and the optimizer step are Operations; the host does the
  // embedding gather/scatter only. `report` says what ran where; a failure is
  // written there and the weights are left untouched (the return is then the
  // previous loss, not a number this call earned). `device_selector` pins
  // every operation to a device name prefix ("cuda", "remote:devops/…").
  struct GraphTrainReport {
    uint64_t operations = 0;
    uint64_t elapsed_ns = 0;
    std::map<std::string, uint64_t> per_device;
    std::string failure;
  };
  float train_step_graph(const std::vector<std::vector<int>>& batch, float lr,
                         Scheduler& scheduler, MemoryFabric& memory, IdGenerator& ids,
                         const EntityID& owner, GraphTrainReport* report = nullptr,
                         const std::string& device_selector = "");
  // The analytic gradient for one sequence, added into `g`, which is laid out
  // exactly like params(); returns that sequence's loss. Public so the backward
  // pass can be CHECKED against the function it claims to differentiate — a
  // hand-written derivative nobody verified is only a plausible derivative.
  float accumulate_gradients(const std::vector<int>& ids, std::vector<std::vector<float>>& g) const;
  std::vector<std::vector<float>> zero_gradients() const;
  // The parameter pointers, for the gradient check to perturb one at a time.
  std::vector<float*> params_for_check() { return params(); }
  std::vector<size_t> param_sizes() const;
  // Greedy/low-temperature continuation.
  // seed 0 = greedy (the mode); any other seed samples with that seed. Stops
  // after emitting any token in stop_ids (that token is included).
  std::vector<int> generate(const std::vector<int>& context_ids, int max_new, const std::vector<int>& stop_ids,
                            uint32_t seed) const;

  void serialize(std::string& out) const;
  bool deserialize(const std::string& blob, size_t& off);

 private:
  struct Layer {
    std::vector<float> g1, b1, g2, b2;              // layernorms
    std::vector<float> Wq, Wk, Wv, Wo;              // [d*d]
    std::vector<float> bq, bk, bv, bo;
    std::vector<float> W1, b1f, W2, b2f;            // ffn
  };
  // Everything the backward pass needs from one forward pass.
  struct Cache;

  void forward(const std::vector<int>& ids, Cache& c) const;

  TinyLmConfig cfg_{};
  int vocab_ = 0;
  std::vector<float> tok_emb_, pos_emb_, gf_, bf_, head_, head_b_;
  std::vector<Layer> layers_;
  // AdamW moments, flattened alongside a parameter pointer table built on demand
  std::vector<float> m_, v_;
  uint64_t steps_ = 0;
  float last_loss_ = 0.0f;

  std::vector<float*> params();
  std::vector<const float*> params() const;
};

// ---- the language path -----------------------------------------------------
struct LanguageReply {
  std::string command;        // the console command the model mapped the request to
  std::string explanation;    // a sentence about the current state vector
  std::string generated;      // the model's raw sampled continuation, verbatim
  bool used_model = false;    // false = the keyword fallback answered, not the model
  float command_nll = 0.0f;   // negative log-likelihood the mapping was chosen by
};

// The resident model. Builds its own corpus from the machine's command surface
// and telemetry vocabulary, trains, persists, and answers.
class MachineLanguageModel {
 public:
  // Loads persisted weights if the store has them; otherwise builds the corpus
  // and trains. Safe to call repeatedly — it only does the work once.
  bool ensure_ready(std::string* what_happened = nullptr);
  bool have_weights() const { return trained_; }
  const ByteBpeTokenizer& tokenizer() const { return tok_; }
  const TinyTransformerLM& model() const { return lm_; }
  size_t corpus_lines() const { return corpus_.size(); }
  const std::vector<std::string>& corpus() const { return corpus_; }
  const std::string& status_note() const { return note_; }
  // The corpus these weights were trained on. Weights whose fingerprint does
  // not match the corpus in front of them are stale and get retrained.
  uint64_t corpus_fingerprint() const { return corpus_fingerprint_; }

  LanguageReply answer(const std::string& request);

 private:
  void build_corpus();
  std::string state_sentence() const;

  ByteBpeTokenizer tok_;
  TinyTransformerLM lm_;
  std::vector<std::string> corpus_;
  std::vector<std::string> commands_;
  bool trained_ = false;
  uint64_t corpus_fingerprint_ = 0;
  std::string note_;
};

MachineLanguageModel* get_machine_language_model();
// The corpus this machine wrote about itself, so a caller can train on the same
// text without owning it, and the weights written back out through the store.
const std::vector<std::string>& machine_corpus();
void persist_machine_language_model();

// The backward pass against central finite differences. If the maths is wrong,
// the relative error is not small, and this is what says so.
struct GradientCheck {
  struct Entry { std::string what; float analytic = 0.0f, numeric = 0.0f, rel_err = 0.0f; };
  std::vector<Entry> entries;
  float worst = 0.0f;         // over the entries a float32 difference can resolve
  int skipped = 0;            // gradients below that resolution: unmeasurable, not wrong
  float floor = 0.0f;         // the resolution itself, in derivative units
  // The decisive test. A single random direction over EVERY parameter at once:
  // the directional derivative is a sum of thousands of terms, so it stands
  // far above the floating-point noise that swamps any one of them, and it is
  // wrong if any part of the backward pass is wrong.
  // numeric_half is the same difference at half the step; their spread is the
  // numeric side's own uncertainty. A direction agrees when
  //   |numeric - analytic| <= 2% of the magnitude + float32 floor + 2 x spread.
  struct Direction {
    float analytic = 0.0f, numeric = 0.0f, numeric_half = 0.0f, rel_err = 0.0f, tolerance = 0.0f;
    bool agrees = false;
  };
  std::vector<Direction> directions;
  float dir_worst = 0.0f;
  bool dir_pass = false;
  int sabotaged = -1;         // the slot whose gradient was negated on purpose, or -1
};
// sabotage_slot >= 0 negates that parameter tensor's analytic gradient first,
// so the check can be seen to fail on a backward pass known to be wrong.
GradientCheck gradient_check(TinyTransformerLM& lm, const std::vector<int>& ids, int samples, int sabotage_slot = -1);

// The whole language path, model first and keyword chain second. `used_model`
// on the reply says which one answered; nothing here guesses.
LanguageReply translate_request(const std::string& input);

// The explicitly-labelled fallback: the historical if/else over keywords.
std::string keyword_fallback_command(const std::string& input);

}  // namespace aios::intelligence
