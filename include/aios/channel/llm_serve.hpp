#pragma once
// An OpenAI-compatible chat endpoint served by the in-tree language model:
// GET /v1/models, GET /health and POST /v1/chat/completions (plain or
// "stream": true as server-sent events). Anything that speaks to a chat-
// completions server (QwenClient, `aios models`, curl) can use it unchanged.
#include <atomic>
#include <functional>
#include <string>
#include "aios/core/result.hpp"
#include "aios/hal/host.hpp"
#include "aios/userland/llm.hpp"

namespace aios::channel {

struct LlmServeOptions {
  uint16_t port = 8000;
  bool loopback_only = true;
  std::string api_key;            // empty: no bearer required (loopback only)
  std::string model_name = "spark";
  int io_timeout_ms = 30000;
  size_t max_body = 4u << 20;
};

// Serves until *stop is set. Requests are answered one generation at a time.
Result<void> llm_serve(userland::LlmModel& model, hal::Host& host, const LlmServeOptions& o,
                       std::function<void(const std::string&)> log, std::atomic<bool>* stop);

}  // namespace aios::channel
