// model_controller.h — the machine's inventory of language models it can
// actually reach, discovered from the endpoints themselves. A model is in the
// list because an endpoint answered and named it, with the size the endpoint
// reported; nothing here is seeded from a table of names somebody believed.
//
// This does not replace `mind` (cli/commands.cpp, cli/console.cpp),
// which hot-swaps the real Intelligence engine bound to a kernel entity. It
// is what `aios models` prints, and what the qwen/mind commands ask when a
// model name arrives without a URL: which host serves it.
#pragma once

#include <string>
#include <vector>
#include <stdexcept>
#include <cstdint>

namespace aios::cli {

// An endpoint that may serve models: an ollama daemon or an OpenAI-compatible
// server (vLLM, the spark serve_llm.py). `base` is scheme://host:port.
struct ModelEndpoint {
  std::string kind;   // "ollama" | "openai"
  std::string base;
  std::string label;  // where it is, for a human ("arch", "devops")
};

struct ModelInfo {
  std::string name;        // as the endpoint names it ("llava:7b", "spark")
  std::string provider;    // endpoint kind
  std::string endpoint;    // chat-completions URL that serves it
  std::string host;        // the endpoint's label
  std::string parameters;  // "7B" when the endpoint says; empty otherwise
  uint64_t bytes = 0;      // on-disk size when the endpoint says
  bool loaded = false;     // the endpoint reports it resident in memory right now
};

struct EndpointStatus {
  ModelEndpoint endpoint;
  bool reachable = false;
  std::string detail;      // HTTP status or the transport error
  size_t models = 0;
};

// The roster: AIOS_MODEL_ENDPOINTS="ollama=http://h:11434,openai=http://h:8000"
// when set, else the fleet's known servers.
std::vector<ModelEndpoint> model_endpoints();

class ModelController {
 public:
  // Probes every endpoint (timeout per endpoint in ms) and fills the list.
  // Returns how many endpoints answered.
  size_t discover(int timeout_ms = 4000);

  const std::vector<ModelInfo>& models() const { return models_; }
  const std::vector<EndpointStatus>& endpoints() const { return endpoints_; }

  // The chat-completions URL of the endpoint that lists `name`; empty when
  // no reachable endpoint does. `where` receives the host label.
  std::string endpoint_for(const std::string& name, std::string* where = nullptr) const;

  // The inventory as text, one line per model, then the endpoints.
  std::string render() const;

 private:
  std::vector<ModelInfo> models_;
  std::vector<EndpointStatus> endpoints_;
};

// One process-wide discovery, on first use: the qwen/mind commands call this
// with a model name and get the URL of the host that serves it. `note`
// receives a sentence about what was found (or not).
std::string resolve_model_endpoint(const std::string& model, std::string* note = nullptr);

}  // namespace aios::cli
