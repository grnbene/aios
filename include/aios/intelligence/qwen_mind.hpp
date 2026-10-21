#pragma once
#include <memory>
#include <string>
#include <vector>
#include <utility>
#include "aios/core/ids.hpp"
#include "aios/core/result.hpp"
#include "aios/hal/host.hpp"
#include "aios/intelligence/intelligence.hpp"
#include "aios/memory/tensor.hpp"

namespace aios::intelligence {

class QwenClient {
 public:
  // An empty url takes AIOS_MIND_URL from the environment, and with neither
  // every call fails saying so. The url "local" runs the in-tree model
  // (llm.hpp) in this process instead of calling a server.
  QwenClient(std::string url = "", std::string model = "spark");

  void set_endpoint(const std::string& url, const std::string& model);
  const std::string& url() const { return url_; }
  const std::string& model() const { return model_; }

  // A bearer credential for the endpoint. Without one this client could only
  // ever reach something unauthenticated — a vLLM or ollama box on the LAN —
  // which is why a bare-metal machine had no way to reach a real model API
  // over its own NIC. Set out of band, never from a command line: an argument
  // ends up in the console transcript, the shell pane and session.log.
  void set_credential(const std::string& bearer) { key_ = bearer; }
  bool has_credential() const { return !key_.empty(); }
  // Some APIs refuse a request that does not bound the reply. 0 leaves it out.
  void set_max_tokens(int n) { max_tokens_ = n; }

  Result<std::string> chat(const std::vector<std::pair<std::string, std::string>>& messages,
                           hal::Host* host = nullptr, const std::string& kernel_root = "",
                           int timeout_ms = 30000) const;

  Result<std::string> complete(const std::string& input_text,
                              hal::Host* host = nullptr, const std::string& kernel_root = "",
                              int timeout_ms = 30000) const;

  Result<std::string> translate_command(const std::string& input_text,
                                        hal::Host* host = nullptr, const std::string& kernel_root = "",
                                        int timeout_ms = 30000) const;

 private:
  std::string url_;
  std::string model_;
  std::string key_;
  int max_tokens_ = 16384;   // a dream's surface is one reply
};

// Reads the credential this machine keeps for its network mind, without it
// ever passing through a command line: <root>/mind.key, else AIOS_MIND_KEY.
// Empty when there is none, which is not an error — an unauthenticated
// endpoint on the LAN is a perfectly good mind.
std::string network_mind_credential(hal::Host& host, const std::string& kernel_root);

class QwenIntelligence final : public Intelligence {
 public:
  QwenIntelligence(std::string url = "", std::string model = "spark");   // "" = AIOS_MIND_URL
  ~QwenIntelligence() override = default;

  std::string name() const override;
  QwenClient& client() { return client_; }
  Result<StateID> perceive(Surface& s, const StateObject& context) override;
  Result<GraphID> think(Surface& s, const StateObject& perceived) override;
  Result<StateID> act(Surface& s, const StateObject& perceived, const ComputeGraph* graph,
                      const ExecutionReport* report) override;

 private:
  QwenClient client_;
  std::string last_input_;
  std::string last_response_;

  // Set by think() when the model's reply asked for a real computation (a
  // "CALC:" directive line) and the entity's own authority was enough to
  // build the graph. act() reads the executed result back from here. Never
  // persists across a cycle where it wasn't set fresh.
  bool has_calc_ = false;
  Tensor calc_out_;
  double calc_a_ = 0, calc_b_ = 0;
  std::string calc_expr_;
};


std::unique_ptr<Intelligence> make_qwen_intelligence(
    const std::string& url = "",   // "" = AIOS_MIND_URL; "local" = the in-tree model
    const std::string& model = "spark");
// Hands a credential to a network mind made above. Separate from the factory
// so the key is never a parameter anything logs.
void set_qwen_credential(Intelligence& mind, const std::string& bearer);

}  // namespace aios::intelligence
