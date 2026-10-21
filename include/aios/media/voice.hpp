#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include "aios/core/result.hpp"
#include "aios/media/audio.hpp"
#include "aios/media/stt.hpp"
#include "aios/media/tts.hpp"

namespace aios::media {

struct VoiceConfig {
  std::string voice_id = "default";
  uint32_t sample_rate = 16000;
  float volume = 1.0f;
  float gain = 1.0f;
  bool auto_speak_response = true;
};

struct VoiceUtterance {
  std::string speaker; // "user" or "aios"
  std::string text;
  AudioBuffer audio;
  uint64_t timestamp_ns = 0;
};

enum class VoiceState : uint8_t {
  Idle = 0,
  Listening = 1,
  Transcribing = 2,
  Thinking = 3,
  Synthesizing = 4,
  Speaking = 5,
  Error = 6
};

const char* voice_state_to_string(VoiceState state);

class VoiceEngine {
 public:
  VoiceEngine() = default;
  ~VoiceEngine() = default;

  Result<void> initialize(const VoiceConfig& config = {});
  
  // High-level duplex Voice functions
  Result<std::string> listen(uint32_t duration_ms = 3000);
  Result<AudioBuffer> speak(const std::string& text);
  Result<VoiceUtterance> process_voice_interaction(const std::string& user_utterance);
  Result<VoiceUtterance> process_ambient_query(const std::string& user_utterance);
  Result<VoiceUtterance> run_duplex_cycle(uint32_t listen_duration_ms = 3000);
  Result<uint64_t> pull_shared_state(float* state_out = nullptr, float* action_out = nullptr, float* predicted_out = nullptr);

  VoiceState state() const { return state_; }
  VoiceConfig config() const { return config_; }
  const std::vector<VoiceUtterance>& history() const { return history_; }
  
  STTEngine& stt() { return stt_; }
  TTSEngine& tts() { return tts_; }
  AudioRecorder& recorder() { return recorder_; }
  AudioPlayer& player() { return player_; }

  void clear_history() { history_.clear(); }

  // Who answers. The voice engine carries an utterance to a mind and speaks
  // what comes back; it has no opinions of its own, and a machine with nothing
  // bound here says so rather than reciting a stock reply.
  using Responder = std::function<Result<std::string>(const std::string& heard)>;
  void set_responder(Responder r) { responder_ = std::move(r); }
  bool has_responder() const { return static_cast<bool>(responder_); }
  // Empty when this machine can speak; otherwise why it cannot.
  const std::string& synthesis_error() const { return synthesis_error_; }
  bool can_speak() const { return synthesis_error_.empty(); }

 private:
  Responder responder_;
  std::string synthesis_error_;
  VoiceConfig config_;
  VoiceState state_ = VoiceState::Idle;
  STTEngine stt_;
  TTSEngine tts_;
  AudioRecorder recorder_;
  AudioPlayer player_;
  std::vector<VoiceUtterance> history_;
};

}  // namespace aios::media
