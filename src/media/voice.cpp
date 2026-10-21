#include <atomic>
#include "aios/media/voice.hpp"
#include "aios/hal/audio.hpp"
#include "aios/intelligence/qwen_mind.hpp"
#include "aios/intelligence/world_model.hpp"
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace aios::media {

const char* voice_state_to_string(VoiceState state) {
  switch (state) {
    case VoiceState::Idle:         return "Idle";
    case VoiceState::Listening:    return "Listening";
    case VoiceState::Transcribing: return "Transcribing";
    case VoiceState::Thinking:     return "Thinking";
    case VoiceState::Synthesizing: return "Synthesizing";
    case VoiceState::Speaking:     return "Speaking";
    case VoiceState::Error:        return "Error";
  }
  return "Unknown";
}

Result<void> VoiceEngine::initialize(const VoiceConfig& config) {
  config_ = config;
  
  STTConfig stt_cfg;
  stt_cfg.sample_rate = config.sample_rate;
  auto stt_init = stt_.initialize(stt_cfg);
  if (!stt_init.ok()) return stt_init.error();

  TTSConfig tts_cfg;
  tts_cfg.sample_rate = config.sample_rate > 0 ? config.sample_rate : 22050;
  tts_cfg.voice_id = config.voice_id;
  tts_cfg.volume = config.volume;
  // A machine that can hear but not speak is a real machine, and so is the
  // other way round. Refusing to come up at all because one half is missing
  // took the whole voice subsystem down over a missing synthesizer.
  auto tts_init = tts_.initialize(tts_cfg);
  synthesis_error_ = tts_init.ok() ? std::string() : tts_init.error().message;

  AudioSpec rec_spec{config.sample_rate, 1, AudioSampleFormat::PCM_S16LE};
  auto rec_open = recorder_.open(rec_spec);
  if (!rec_open.ok()) return rec_open.error();
  recorder_.set_gain(config.gain);

  AudioSpec play_spec{tts_cfg.sample_rate, 1, AudioSampleFormat::PCM_S16LE};
  auto play_open = player_.open(play_spec);
  if (!play_open.ok()) return play_open.error();
  player_.set_volume(config.volume);

  state_ = VoiceState::Idle;
  return Ok();
}

Result<std::string> VoiceEngine::listen(uint32_t duration_ms) {
  if (state_ == VoiceState::Error) {
    return Result<std::string>(Status::Invalid, "VoiceEngine is in error state");
  }

  state_ = VoiceState::Listening;
  auto start_rec = recorder_.start_recording();
  if (!start_rec.ok()) {
    state_ = VoiceState::Error;
    return Result<std::string>(start_rec.error());
  }

  size_t target_frames = static_cast<size_t>((static_cast<uint64_t>(duration_ms) * recorder_.spec().sample_rate) / 1000);
  if (target_frames == 0) target_frames = recorder_.spec().sample_rate;

  // The microphone. What used to be here filled the buffer with a 440Hz sine
  // wave whenever the hardware had given nothing, and the transcriber
  // downstream duly "heard" it — so a machine with no microphone at all
  // reported that it had listened.
  if (recorder_.available_frames() < target_frames) {
    size_t missing = target_frames - recorder_.available_frames();
    auto captured = hal::audio_capture(recorder_.spec().sample_rate, missing, duration_ms + 3000);
    if (!captured.ok()) {
      (void)recorder_.stop_recording();
      state_ = VoiceState::Error;
      return Result<std::string>(captured.error());
    }
    AudioBuffer rec_input;
    rec_input.spec = recorder_.spec();
    rec_input.data.resize(captured->size() * sizeof(int16_t));
    std::memcpy(rec_input.data.data(), captured->data(), rec_input.data.size());
    (void)recorder_.push_samples(rec_input);
  }

  auto read_res = recorder_.read_samples(target_frames);
  (void)recorder_.stop_recording();

  if (!read_res.ok()) {
    state_ = VoiceState::Error;
    return Result<std::string>(read_res.error());
  }

  state_ = VoiceState::Transcribing;
  auto stt_res = stt_.transcribe(*read_res);
  if (!stt_res.ok()) {
    state_ = VoiceState::Error;
    return Result<std::string>(stt_res.error());
  }

  // An empty transcript means nothing was said. It used to be replaced with
  // "AIOS voice listening active", which reads like the machine heard that.
  std::string transcript = stt_res->transcript;

  VoiceUtterance u;
  u.speaker = "user";
  u.text = transcript;
  u.audio = *read_res;
  u.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  history_.push_back(u);

  state_ = VoiceState::Idle;
  return transcript;
}

Result<AudioBuffer> VoiceEngine::speak(const std::string& text) {
  if (text.empty()) {
    return AudioBuffer{};
  }
  if (!synthesis_error_.empty()) {
    return Result<AudioBuffer>(Status::Unsupported, synthesis_error_);
  }

  state_ = VoiceState::Synthesizing;
  auto tts_res = tts_.synthesize(text);
  if (!tts_res.ok()) {
    state_ = VoiceState::Error;
    return Result<AudioBuffer>(tts_res.error());
  }

  state_ = VoiceState::Speaking;
  (void)player_.play();
  auto write_res = player_.write_samples(tts_res->audio);
  if (!write_res.ok()) {
    state_ = VoiceState::Error;
    return Result<AudioBuffer>(write_res.error());
  }
  // And out of the speaker. Staging the samples in a buffer and calling that
  // "played to speaker output" is what the command surface used to report.
  auto staged = player_.get_playback_output();
  if (staged.ok() && !staged->data.empty() &&
      staged->spec.format == AudioSampleFormat::PCM_S16LE) {
    const int16_t* pcm = reinterpret_cast<const int16_t*>(staged->data.data());
    size_t frames = staged->data.size() / sizeof(int16_t);
    auto out = hal::audio_play(pcm, frames, staged->spec.sample_rate);
    if (!out.ok()) {
      state_ = VoiceState::Error;
      return Result<AudioBuffer>(out.error());
    }
  }

  VoiceUtterance u;
  u.speaker = "aios";
  u.text = text;
  u.audio = tts_res->audio;
  u.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  history_.push_back(u);

  state_ = VoiceState::Idle;
  return tts_res->audio;
}

Result<VoiceUtterance> VoiceEngine::process_voice_interaction(const std::string& user_utterance) {
  state_ = VoiceState::Thinking;

  // Zero-latency pull from SharedStateVectorRegion to ensure duplex voice engine is synchronized with continuous substrate state
  (void)pull_shared_state();

  // What to say back. This engine does not have a mind of its own: it carries
  // an utterance to whoever does and speaks the answer. The canned replies that
  // used to live here ("AIOS voice subsystem is online and fully operational.
  // All kernel loops verified.") were answers to questions nobody had asked.
  std::string response_text;
  if (user_utterance.empty()) {
    state_ = VoiceState::Idle;
    return Result<VoiceUtterance>(Status::Invalid, "nothing was said, so there is nothing to answer");
  }
  if (responder_) {
    auto answered = responder_(user_utterance);
    if (!answered.ok()) { state_ = VoiceState::Error; return answered.error(); }
    response_text = *answered;
  } else {
    state_ = VoiceState::Idle;
    return Result<VoiceUtterance>(Status::Unsupported,
        "no mind is bound to this voice engine; bind one with set_responder before asking it to converse");
  }

  if (config_.auto_speak_response) {
    auto speak_res = speak(response_text);
    if (!speak_res.ok()) {
      return Result<VoiceUtterance>(speak_res.error());
    }
  }

  VoiceUtterance u;
  u.speaker = "aios";
  u.text = response_text;
  u.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  
  return u;
}

Result<VoiceUtterance> VoiceEngine::process_ambient_query(const std::string& user_utterance) {
  return process_voice_interaction(user_utterance);
}

Result<VoiceUtterance> VoiceEngine::run_duplex_cycle(uint32_t listen_duration_ms) {
  auto listen_res = listen(listen_duration_ms);
  if (!listen_res.ok()) {
    return Result<VoiceUtterance>(listen_res.error());
  }

  return process_voice_interaction(*listen_res);
}

Result<uint64_t> VoiceEngine::pull_shared_state(float* state_out, float* action_out, float* predicted_out) {
  auto* region = intelligence::get_global_shared_state_region();
  if (!region) {
    return Result<uint64_t>(Status::NotFound, "SharedStateVectorRegion offline");
  }

  // Seqlock reader: copy, then confirm the count did not move and was even.
  // Retry until a copy is clean — one retry, as before, still handed back a
  // torn copy whenever the writer was in the middle of two updates in a row.
  // Bounded, so a writer that died mid-update is an error, not a hang.
  for (uint64_t attempt = 0; attempt < 1000000; ++attempt) {
    uint64_t seq1 = region->sequence.load(std::memory_order_acquire);
    if (seq1 & 1) continue;                       // a write is in progress
    if (state_out) std::copy_n(region->state_vector, intelligence::kSubstrateStateDim, state_out);
    if (action_out) std::copy_n(region->action_vector, intelligence::kActionVectorDim, action_out);
    if (predicted_out) std::copy_n(region->predicted_next_state, intelligence::kSubstrateStateDim, predicted_out);
    std::atomic_thread_fence(std::memory_order_acquire);   // the copies above happen before the re-read
    uint64_t seq2 = region->sequence.load(std::memory_order_relaxed);
    if (seq1 == seq2) return seq2;
  }
  return Result<uint64_t>(Status::Busy, "the shared state region never held still long enough to copy (a writer stuck mid-update?)");
}

}  // namespace aios::media
