// Speech synthesis. The engine is the substrate's, reached through
// hal::speech_synthesize; this layer is the buffering and the format around it.
//
// It used to be a sine generator: 220Hz plus a third of its second harmonic
// under a half-sine envelope, for any text, in any of three invented "voices"
// named Alex and Samantha that made no difference to a single sample. It
// sounded like a phone ringing and it was reported as speech.
#include "aios/media/tts.hpp"
#include "aios/hal/audio.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace aios::media {

Result<void> TTSEngine::initialize(const TTSConfig& config) {
  config_ = config;
  voices_.clear();
  auto voices = hal::speech_voices();
  if (!voices.ok()) {
    state_ = TTSState::Error;
    return voices.error();
  }
  for (const auto& v : *voices) {
    voices_.push_back(TTSVoice{v.id, v.name, v.language, v.gender});
  }
  if (voices_.empty()) {
    state_ = TTSState::Error;
    return Result<void>(Status::Unsupported, "the substrate's speech engine reports no voices");
  }
  // "default" is not a voice any engine has; it means the one the engine picks.
  if (config_.voice_id == "default") config_.voice_id.clear();
  state_ = TTSState::Ready;
  return Ok();
}

std::vector<TTSVoice> TTSEngine::available_voices() const {
  return voices_;
}

Result<void> TTSEngine::set_voice(const std::string& voice_id) {
  if (voice_id.empty() || voice_id == "default") { config_.voice_id.clear(); return Ok(); }
  auto it = std::find_if(voices_.begin(), voices_.end(), [&](const TTSVoice& v) {
    return v.id == voice_id || v.name == voice_id;
  });
  if (it == voices_.end()) {
    return Result<void>(Status::NotFound, "this machine's speech engine has no voice '" + voice_id + "'");
  }
  config_.voice_id = it->id;
  return Ok();
}

Result<void> TTSEngine::set_speaking_rate(float rate) {
  if (rate < 0.2f || rate > 5.0f) {
    return Result<void>(Status::Invalid, "Speaking rate out of range (0.2 - 5.0)");
  }
  config_.speaking_rate = rate;
  return Ok();
}

Result<void> TTSEngine::set_pitch(float pitch) {
  if (pitch < 0.2f || pitch > 5.0f) {
    return Result<void>(Status::Invalid, "Pitch out of range (0.2 - 5.0)");
  }
  config_.pitch = pitch;
  return Ok();
}

Result<TTSResult> TTSEngine::synthesize(const std::string& text) {
  if (state_ == TTSState::Uninitialized) {
    return Result<TTSResult>(Status::Invalid, "TTSEngine not initialized");
  }
  if (text.empty()) {
    TTSResult res;
    res.audio.spec.sample_rate = config_.sample_rate;
    res.audio.spec.channels = 1;
    res.audio.spec.format = config_.format;
    return res;
  }

  state_ = TTSState::Synthesizing;
  auto start_time = std::chrono::steady_clock::now();

  TTSResult result;
  result.character_count = text.size();
  auto audio = generate_pcm_for_text(text);
  if (!audio.ok()) { state_ = TTSState::Error; return audio.error(); }
  result.audio = std::move(*audio);

  auto end_time = std::chrono::steady_clock::now();
  result.synthesis_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

  state_ = TTSState::Ready;
  return result;
}

Result<std::vector<AudioBuffer>> TTSEngine::synthesize_stream(const std::string& text, size_t chunk_size_chars) {
  if (state_ == TTSState::Uninitialized) {
    return Result<std::vector<AudioBuffer>>(Status::Invalid, "TTSEngine not initialized");
  }
  if (chunk_size_chars == 0) chunk_size_chars = 64;

  std::vector<AudioBuffer> chunks;
  if (text.empty()) return chunks;

  for (size_t i = 0; i < text.size(); i += chunk_size_chars) {
    std::string chunk_text = text.substr(i, chunk_size_chars);
    auto chunk = generate_pcm_for_text(chunk_text);
    if (!chunk.ok()) return chunk.error();
    chunks.push_back(std::move(*chunk));
  }

  return chunks;
}

void TTSEngine::reset() {
  state_ = TTSState::Uninitialized;
  voices_.clear();
}

Result<AudioBuffer> TTSEngine::generate_pcm_for_text(const std::string& text) {
  // The substrate's speech engine says how fast it can speak; the config's
  // rate is a multiple of the engine's own default, which for both `say` and
  // espeak-ng is around 175 words a minute.
  constexpr double kDefaultWpm = 175.0;
  float rate = config_.speaking_rate > 0.05f ? config_.speaking_rate : 1.0f;
  uint32_t wpm = static_cast<uint32_t>(std::clamp(kDefaultWpm * rate, 60.0, 700.0));
  uint32_t want_rate = config_.sample_rate > 0 ? config_.sample_rate : 22050;

  auto spoken = hal::speech_synthesize(text, config_.voice_id, wpm, want_rate);
  if (!spoken.ok()) return spoken.error();

  AudioBuffer buffer;
  buffer.spec.sample_rate = spoken->sample_rate ? spoken->sample_rate : want_rate;
  buffer.spec.channels = 1;
  buffer.spec.format = config_.format;

  float volume = std::clamp(config_.volume, 0.0f, 1.0f);
  const std::vector<int16_t>& pcm = spoken->pcm;

  if (config_.format == AudioSampleFormat::PCM_S16LE) {
    buffer.data.resize(pcm.size() * sizeof(int16_t));
    int16_t* out = reinterpret_cast<int16_t*>(buffer.data.data());
    for (size_t i = 0; i < pcm.size(); ++i) {
      out[i] = static_cast<int16_t>(std::clamp(pcm[i] * volume, -32768.0f, 32767.0f));
    }
  } else if (config_.format == AudioSampleFormat::PCM_U8) {
    buffer.data.resize(pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i) {
      float v = (pcm[i] * volume) / 256.0f + 128.0f;
      buffer.data[i] = static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f));
    }
  } else {
    buffer.spec.format = AudioSampleFormat::PCM_S16LE;
    buffer.data.resize(pcm.size() * sizeof(int16_t));
    std::memcpy(buffer.data.data(), pcm.data(), buffer.data.size());
  }
  return buffer;
}

}  // namespace aios::media
