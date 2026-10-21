// Drives the in-tree language model on a real checkpoint: aios-llm-drive
// <model-dir> [adapter=<dir>] [tokens|logits|chat] <text>. "tokens" prints the
// ids the tokenizer gives for the text, "logits" the five likeliest next tokens,
// "chat" (the default) wraps the text as a user turn and generates greedily.
#include <cstdio>
#include <cstring>
#include <string>
#include "aios/hal/host.hpp"
#include "aios/userland/llm.hpp"

using namespace aios;
using namespace aios::userland;

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: aios-llm-drive <model-dir> [adapter=<dir>] [tokens|logits|chat] <text>\n"); return 2; }
  int a = 1;
  const std::string dir = argv[a++];
  std::string adapter, mode = "chat";
  uint32_t max_tokens = 64;
  for (; a < argc; ++a) {
    if (!std::strncmp(argv[a], "adapter=", 8)) adapter = argv[a] + 8;
    else if (!std::strncmp(argv[a], "max=", 4)) max_tokens = (uint32_t)std::stoul(argv[a] + 4);
    else if (!std::strcmp(argv[a], "tokens") || !std::strcmp(argv[a], "logits") || !std::strcmp(argv[a], "chat")) mode = argv[a];
    else break;
  }
  std::string text;
  for (; a < argc; ++a) { if (!text.empty()) text += ' '; text += argv[a]; }
  hal::Host& host = hal::process_host();
  const uint64_t t0 = host.monotonic_ns();
  auto m = LlmModel::load(host, dir, adapter);
  if (!m.ok()) { std::fprintf(stderr, "load: %s\n", m.error().message.c_str()); return 1; }
  std::fprintf(stderr, "loaded %.1f MB of weights in %.2f s%s\n", (double)(*m)->weight_bytes() / 1e6,
               (double)(host.monotonic_ns() - t0) / 1e9, (*m)->has_adapter() ? " (with adapter)" : "");
  if (mode == "tokens") {
    auto ids = (*m)->tokenizer().encode(text);
    for (size_t i = 0; i < ids.size(); ++i) std::printf("%d%s", ids[i], i + 1 < ids.size() ? " " : "\n");
    std::string back = (*m)->tokenizer().decode(ids);
    return back == text ? 0 : (std::fprintf(stderr, "round trip differs\n"), 1);
  }
  if (mode == "logits") {
    auto lg = (*m)->logits((*m)->tokenizer().encode(text));
    if (!lg.ok()) { std::fprintf(stderr, "%s\n", lg.error().message.c_str()); return 1; }
    for (int k = 0; k < 5; ++k) {
      size_t best = 0;
      for (size_t i = 1; i < lg->size(); ++i) if ((*lg)[i] > (*lg)[best]) best = i;
      std::printf("%zu %.4f %s\n", best, (double)(*lg)[best], (*m)->tokenizer().token_bytes((int32_t)best).c_str());
      (*lg)[best] = -1e30f;
    }
    return 0;
  }
  SampleParams sp = (*m)->defaults();   // the checkpoint's own settings, decoded greedily
  sp.temperature = 0.f;
  sp.max_tokens = max_tokens;
  auto r = (*m)->generate(host, (*m)->chat_prompt({{"user", text}}), sp, [](const std::string& s) { std::fputs(s.c_str(), stdout); std::fflush(stdout); return true; });
  if (!r.ok()) { std::fprintf(stderr, "\n%s\n", r.error().message.c_str()); return 1; }
  std::printf("\n");
  std::fprintf(stderr, "%u prompt tokens in %.0f ms, %u tokens in %.0f ms (%.2f tok/s)\n", r->prompt_tokens, r->prefill_ms,
               r->completion_tokens, r->decode_ms, r->completion_tokens ? r->completion_tokens / (r->decode_ms / 1e3) : 0.0);
  return 0;
}
