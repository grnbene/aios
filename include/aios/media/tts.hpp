#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "aios/core/result.hpp"
#include "aios/media/audio.hpp"

namespace aios::media {

struct TTSVoice {
  std::string id;
  std::string name;
  std::string language;
  std::string gender;
};

struct TTSConfig {
  std::string voice_id = "default";
  float speaking_rate = 1.0f;
  float pitch = 1.0f;
  float volume = 1.0f;
  uint32_t sample_rate = 22050;
  AudioSampleFormat format = AudioSampleFormat::PCM_S16LE;
};

struct TTSResult {
  AudioBuffer audio;
  uint64_t synthesis_time_ms = 0;
  size_t character_count = 0;
};

enum class TTSState : uint8_t {
  Uninitialized = 0,
  Ready = 1,
  Synthesizing = 2,
  Error = 3
};

class TTSEngine {
 public:
  TTSEngine() = default;
  ~TTSEngine() = default;

  Result<void> initialize(const TTSConfig& config = {});
  Result<TTSResult> synthesize(const std::string& text);
  Result<std::vector<AudioBuffer>> synthesize_stream(const std::string& text, size_t chunk_size_chars = 64);

  std::vector<TTSVoice> available_voices() const;
  Result<void> set_voice(const std::string& voice_id);
  Result<void> set_speaking_rate(float rate);
  Result<void> set_pitch(float pitch);

  TTSState state() const { return state_; }
  TTSConfig config() const { return config_; }
  void reset();

 private:
  TTSConfig config_;
  TTSState state_ = TTSState::Uninitialized;
  std::vector<TTSVoice> voices_;

  // Synthesis can fail — there may be no engine on this substrate, or it may
  // refuse the text — and a caller must be told rather than handed a buffer.
  Result<AudioBuffer> generate_pcm_for_text(const std::string& text);
};

}  // namespace aios::media
