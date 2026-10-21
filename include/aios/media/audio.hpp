#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "aios/core/result.hpp"
#include "aios/hal/sync.hpp"

namespace aios::media {

enum class AudioSampleFormat : uint8_t {
  PCM_S16LE = 0,
  PCM_F32LE = 1,
  PCM_U8 = 2
};

const char* audio_sample_format_to_string(AudioSampleFormat fmt);

struct AudioSpec {
  uint32_t sample_rate = 44100;
  uint8_t channels = 2;
  AudioSampleFormat format = AudioSampleFormat::PCM_S16LE;

  uint32_t bytes_per_sample() const;
  uint32_t frame_size_bytes() const;
};

struct AudioBuffer {
  AudioSpec spec;
  std::vector<uint8_t> data;
  uint64_t timestamp_ns = 0;

  size_t frame_count() const;
  double duration_seconds() const;
};

enum class AudioState : uint8_t {
  Stopped = 0,
  Recording = 1,
  Playing = 2,
  Paused = 3
};

class AudioRecorder {
 public:
  AudioRecorder() = default;
  ~AudioRecorder() = default;

  Result<void> open(const AudioSpec& spec = {});
  Result<void> start_recording();
  Result<void> pause_recording();
  Result<void> stop_recording();

  Result<AudioBuffer> read_samples(size_t max_frames = 0);
  Result<void> push_samples(const AudioBuffer& buffer);

  bool is_recording() const { return state_ == AudioState::Recording; }
  AudioState state() const { return state_; }
  AudioSpec spec() const { return spec_; }
  size_t available_frames() const;
  void set_gain(float gain) { gain_ = gain; }
  float gain() const { return gain_; }

 private:
  AudioSpec spec_;
  AudioState state_ = AudioState::Stopped;
  std::vector<uint8_t> buffer_;
  float gain_ = 1.0f;
  mutable hal::Mutex mutex_;
};

class AudioPlayer {
 public:
  AudioPlayer() = default;
  ~AudioPlayer() = default;

  Result<void> open(const AudioSpec& spec = {});
  Result<void> play();
  Result<void> pause();
  Result<void> stop();

  Result<void> write_samples(const AudioBuffer& buffer);
  Result<AudioBuffer> get_playback_output(size_t max_frames = 0);

  bool is_playing() const { return state_ == AudioState::Playing; }
  AudioState state() const { return state_; }
  AudioSpec spec() const { return spec_; }
  void set_volume(float volume) { volume_ = volume; }
  float volume() const { return volume_; }
  size_t buffered_frames() const;

 private:
  AudioSpec spec_;
  AudioState state_ = AudioState::Stopped;
  std::vector<uint8_t> buffer_;
  float volume_ = 1.0f;
  mutable hal::Mutex mutex_;
};

}  // namespace aios::media
