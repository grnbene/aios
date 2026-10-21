#pragma once
// A Qwen2-architecture decoder-only language model run in-tree: weights read
// from safetensors, an optional LoRA adapter applied on top, a KV cache, and the
// forward pass on the host's lanes. This is what answers a real chat request
// without any outside model server.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "aios/core/result.hpp"
#include "aios/hal/host.hpp"
#include "aios/userland/bpe.hpp"

namespace aios::userland {

struct LlmConfig {
  uint32_t vocab = 0, hidden = 0, layers = 0, heads = 0, kv_heads = 0, ffn = 0;
  uint32_t head_dim = 0, max_positions = 0;
  float rms_eps = 1e-6f, rope_theta = 1e6f;
  bool tied_head = true;
};

struct SampleParams {
  float temperature = 0.0f;   // 0 = greedy
  float top_p = 1.0f;
  uint32_t top_k = 0;         // 0 = no cut
  float repetition_penalty = 1.0f;   // > 1 discourages every token already in the context
  uint64_t seed = 0;          // 0 = take one from the host
  uint32_t max_tokens = 256;
  std::vector<std::string> stop;
};

struct ChatMessage { std::string role, content; };

struct GenerateResult {
  std::string text;
  uint32_t prompt_tokens = 0, completion_tokens = 0;
  bool hit_limit = false;   // stopped on max_tokens (finish_reason "length")
  bool cancelled = false;   // on_text asked to stop
  double prefill_ms = 0, decode_ms = 0;
};

class LlmModel {
 public:
  // dir holds config.json, model.safetensors (or sharded files) and
  // tokenizer.json. adapter_dir, when set, holds a PEFT LoRA adapter
  // (adapter_config.json + adapter_model.safetensors) applied unmerged.
  static Result<std::unique_ptr<LlmModel>> load(hal::Host& host, const std::string& dir,
                                                const std::string& adapter_dir = {},
                                                uint32_t context = 4096);
  ~LlmModel();

  const LlmConfig& config() const;
  const BpeTokenizer& tokenizer() const;
  uint32_t context() const;
  bool has_adapter() const;
  uint64_t weight_bytes() const;

  // The checkpoint's chat template for plain turns: its default system turn
  // when none is given, each turn framed with <|im_start|>/<|im_end|>, and the
  // assistant turn left open.
  std::string chat_prompt(const std::vector<ChatMessage>& messages) const;
  const std::string& default_system() const;
  // Sampling defaults the checkpoint ships in generation_config.json.
  const SampleParams& defaults() const;

  // Logits for the last position of `tokens`, run from a fresh cache. For
  // checking the arithmetic against a reference.
  Result<std::vector<float>> logits(const std::vector<int32_t>& tokens);

  // Continue `prompt` (already templated). on_text sees each piece as it is
  // decoded and may return false to stop. Calls from several threads take
  // turns: the model holds one KV cache.
  Result<GenerateResult> generate(hal::Host& host, const std::string& prompt, const SampleParams& p,
                                  const std::function<bool(const std::string&)>& on_text = {});

 private:
  LlmModel();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The model this process runs in-tree, shared by `infer`, the chat endpoint,
// `qwen local` and any mind bound to the url "local". resident_llm loads it on
// first use from AIOS_LLM_MODEL (else <kernel_root>/models/default) with
// AIOS_LLM_ADAPTER; resident_llm_load replaces it. The pointer stays valid
// until the next resident_llm_load.
Result<LlmModel*> resident_llm(hal::Host& host, const std::string& kernel_root);
Result<LlmModel*> resident_llm_load(hal::Host& host, const std::string& dir, const std::string& adapter_dir,
                                    uint32_t context = 4096);
LlmModel* resident_llm_loaded();          // null when nothing is loaded
std::string resident_llm_source();        // "<dir>" or "<dir> + adapter <dir>", empty when none

}  // namespace aios::userland
