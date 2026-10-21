#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "aios/core/result.hpp"
#include "aios/media/audio.hpp"

namespace aios::media {

struct STTConfig {
  std::string model_name = "default";
  std::string language = "en";
  uint32_t sample_rate = 16000;
  bool enable_timestamps = true;
  float confidence_threshold = 0.0f;
};

struct STTToken {
  std::string text;
  float confidence = 1.0f;
  uint64_t start_ms = 0;
  uint64_t end_ms = 0;
};

struct STTResult {
  std::string transcript;
  float confidence = 0.0f;
  std::vector<STTToken> tokens;
  uint64_t processing_time_ms = 0;
};

enum class STTState : uint8_t {
  Uninitialized = 0,
  Ready = 1,
  Processing = 2,
  Error = 3
};

class STTEngine {
 public:
  STTEngine() = default;
  ~STTEngine() = default;

  Result<void> initialize(const STTConfig& config = {});
  Result<STTResult> transcribe(const AudioBuffer& buffer);
  Result<STTResult> transcribe_pcm16(const int16_t* samples, size_t sample_count, uint32_t sample_rate = 16000);

  Result<void> start_stream();
  Result<void> feed_stream(const AudioBuffer& buffer);
  Result<STTResult> finish_stream();

  STTState state() const { return state_; }
  STTConfig config() const { return config_; }
  void reset();

 private:
  STTConfig config_;
  STTState state_ = STTState::Uninitialized;
  AudioBuffer stream_buffer_;
  bool is_streaming_ = false;

  // Recognition can fail — there may be no recogniser on this substrate — and
  // the caller must be told rather than handed a sentence.
  Result<STTResult> process_audio_internal(const int16_t* samples, size_t sample_count, uint32_t rate);
};

}  // namespace aios::media
