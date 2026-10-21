// Speech recognition. The recogniser is the substrate's, reached through
// hal::speech_recognize; this layer converts the audio and carries the result.
//
// It used to look at the audio only to decide whether it was silent, and then
// emit words from a fixed list — "complete input speech recognized system ready
// command task" — one every half second, with confidences of 0.85, 0.95 and
// 0.75 in rotation. Every recording in the world transcribed to the same
// sentence, and the confidence made it look considered.
#include "aios/media/stt.hpp"
#include "aios/hal/audio.hpp"

#include <chrono>
#include <sstream>
#include <numeric>

namespace aios::media {

Result<void> STTEngine::initialize(const STTConfig& config) {
  config_ = config;
  state_ = STTState::Ready;
  stream_buffer_.spec.sample_rate = config.sample_rate;
  stream_buffer_.spec.channels = 1;
  stream_buffer_.spec.format = AudioSampleFormat::PCM_S16LE;
  stream_buffer_.data.clear();
  is_streaming_ = false;
  return Ok();
}

Result<STTResult> STTEngine::transcribe(const AudioBuffer& buffer) {
  if (state_ == STTState::Uninitialized) {
    return Result<STTResult>(Status::Invalid, "STTEngine not initialized");
  }
  if (buffer.data.empty()) {
    return STTResult{};
  }

  state_ = STTState::Processing;

  const int16_t* samples = nullptr;
  size_t sample_count = 0;
  std::vector<int16_t> converted_pcm;

  if (buffer.spec.format == AudioSampleFormat::PCM_S16LE) {
    samples = reinterpret_cast<const int16_t*>(buffer.data.data());
    sample_count = buffer.data.size() / sizeof(int16_t);
  } else if (buffer.spec.format == AudioSampleFormat::PCM_U8) {
    converted_pcm.resize(buffer.data.size());
    for (size_t i = 0; i < buffer.data.size(); ++i) {
      converted_pcm[i] = static_cast<int16_t>((static_cast<int>(buffer.data[i]) - 128) * 256);
    }
    samples = converted_pcm.data();
    sample_count = converted_pcm.size();
  } else {
    state_ = STTState::Ready;
    return Result<STTResult>(Status::Unsupported, "Unsupported audio sample format for STT");
  }

  auto result = process_audio_internal(samples, sample_count, buffer.spec.sample_rate);
  state_ = result.ok() ? STTState::Ready : STTState::Error;
  return result;
}

Result<STTResult> STTEngine::transcribe_pcm16(const int16_t* samples, size_t sample_count, uint32_t sample_rate) {
  if (state_ == STTState::Uninitialized) {
    return Result<STTResult>(Status::Invalid, "STTEngine not initialized");
  }
  if (samples == nullptr || sample_count == 0) {
    return STTResult{};
  }

  state_ = STTState::Processing;
  auto result = process_audio_internal(samples, sample_count, sample_rate);
  state_ = result.ok() ? STTState::Ready : STTState::Error;
  return result;
}

Result<void> STTEngine::start_stream() {
  if (state_ == STTState::Uninitialized) {
    return Result<void>(Status::Invalid, "STTEngine not initialized");
  }
  stream_buffer_.data.clear();
  is_streaming_ = true;
  return Ok();
}

Result<void> STTEngine::feed_stream(const AudioBuffer& buffer) {
  if (!is_streaming_) {
    return Result<void>(Status::Invalid, "STTEngine stream not started");
  }
  if (!buffer.data.empty()) {
    stream_buffer_.data.insert(stream_buffer_.data.end(), buffer.data.begin(), buffer.data.end());
  }
  return Ok();
}

Result<STTResult> STTEngine::finish_stream() {
  if (!is_streaming_) {
    return Result<STTResult>(Status::Invalid, "STTEngine stream not active");
  }
  is_streaming_ = false;
  AudioBuffer buf = stream_buffer_;
  stream_buffer_.data.clear();
  return transcribe(buf);
}

void STTEngine::reset() {
  state_ = STTState::Uninitialized;
  stream_buffer_.data.clear();
  is_streaming_ = false;
}

Result<STTResult> STTEngine::process_audio_internal(const int16_t* samples, size_t sample_count, uint32_t rate) {
  auto start_time = std::chrono::steady_clock::now();
  STTResult result;
  if (sample_count == 0) return result;

  uint64_t duration_ms = static_cast<uint64_t>(sample_count * 1000.0 / (rate > 0 ? rate : 16000));

  // Silence is the one thing this layer can tell on its own, and saying so
  // saves waking a model to hear nothing. Everything else goes to the engine.
  double total_energy = 0.0;
  for (size_t i = 0; i < sample_count; ++i) {
    double v = samples[i] / 32768.0;
    total_energy += v * v;
  }
  if (total_energy / sample_count < 1e-7) {
    result.transcript.clear();
    result.confidence = 0.0f;
    STTToken tok;
    tok.text = "[silence]";
    tok.confidence = 0.0f;
    tok.start_ms = 0;
    tok.end_ms = duration_ms;
    result.tokens.push_back(tok);
    auto done = std::chrono::steady_clock::now();
    result.processing_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(done - start_time).count();
    return result;
  }

  auto heard = hal::speech_recognize(samples, sample_count, rate, config_.language);
  if (!heard.ok()) return heard.error();

  result.transcript = heard->text;
  result.confidence = heard->confidence;
  for (const auto& w : heard->words) {
    STTToken tok;
    tok.text = w.text;
    tok.confidence = w.confidence;
    tok.start_ms = w.start_ms;
    tok.end_ms = w.end_ms;
    result.tokens.push_back(tok);
  }

  auto end_time = std::chrono::steady_clock::now();
  result.processing_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
  return result;
}

}  // namespace aios::media
