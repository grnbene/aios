#include "aios/media/audio.hpp"
#include <algorithm>
#include <cmath>

namespace aios::media {

const char* audio_sample_format_to_string(AudioSampleFormat fmt) {
  switch (fmt) {
    case AudioSampleFormat::PCM_S16LE: return "PCM_S16LE";
    case AudioSampleFormat::PCM_F32LE: return "PCM_F32LE";
    case AudioSampleFormat::PCM_U8:    return "PCM_U8";
  }
  return "Unknown";
}

uint32_t AudioSpec::bytes_per_sample() const {
  switch (format) {
    case AudioSampleFormat::PCM_S16LE: return 2;
    case AudioSampleFormat::PCM_F32LE: return 4;
    case AudioSampleFormat::PCM_U8:    return 1;
  }
  return 2;
}

uint32_t AudioSpec::frame_size_bytes() const {
  return bytes_per_sample() * channels;
}

size_t AudioBuffer::frame_count() const {
  uint32_t frame_bytes = spec.frame_size_bytes();
  return frame_bytes > 0 ? data.size() / frame_bytes : 0;
}

double AudioBuffer::duration_seconds() const {
  if (spec.sample_rate == 0) return 0.0;
  return static_cast<double>(frame_count()) / spec.sample_rate;
}

// AudioRecorder Implementation

Result<void> AudioRecorder::open(const AudioSpec& spec) {
  hal::LockGuard lock(mutex_);
  if (spec.sample_rate == 0 || spec.channels == 0) {
    return Result<void>(Status::Invalid, "Invalid AudioSpec parameters");
  }
  spec_ = spec;
  buffer_.clear();
  state_ = AudioState::Stopped;
  return Ok();
}

Result<void> AudioRecorder::start_recording() {
  hal::LockGuard lock(mutex_);
  if (spec_.sample_rate == 0) {
    return Result<void>(Status::Invalid, "AudioRecorder must be opened before starting");
  }
  state_ = AudioState::Recording;
  return Ok();
}

Result<void> AudioRecorder::pause_recording() {
  hal::LockGuard lock(mutex_);
  if (state_ != AudioState::Recording) {
    return Result<void>(Status::Invalid, "AudioRecorder is not currently recording");
  }
  state_ = AudioState::Paused;
  return Ok();
}

Result<void> AudioRecorder::stop_recording() {
  hal::LockGuard lock(mutex_);
  state_ = AudioState::Stopped;
  return Ok();
}

Result<void> AudioRecorder::push_samples(const AudioBuffer& buffer) {
  hal::LockGuard lock(mutex_);
  if (state_ != AudioState::Recording) {
    return Result<void>(Status::Invalid, "AudioRecorder is not in recording state");
  }
  if (buffer.data.empty()) {
    return Ok();
  }

  size_t start_idx = buffer_.size();
  buffer_.insert(buffer_.end(), buffer.data.begin(), buffer.data.end());

  if (std::abs(gain_ - 1.0f) > 1e-4f && spec_.format == AudioSampleFormat::PCM_S16LE) {
    int16_t* samples = reinterpret_cast<int16_t*>(buffer_.data() + start_idx);
    size_t count = (buffer_.size() - start_idx) / sizeof(int16_t);
    for (size_t i = 0; i < count; ++i) {
      float val = samples[i] * gain_;
      val = std::clamp(val, -32768.0f, 32767.0f);
      samples[i] = static_cast<int16_t>(val);
    }
  }

  return Ok();
}

Result<AudioBuffer> AudioRecorder::read_samples(size_t max_frames) {
  hal::LockGuard lock(mutex_);
  uint32_t frame_bytes = spec_.frame_size_bytes();
  if (frame_bytes == 0) {
    return Result<AudioBuffer>(Status::Invalid, "AudioRecorder spec not set");
  }

  size_t available = buffer_.size() / frame_bytes;
  size_t frames_to_read = (max_frames == 0 || max_frames > available) ? available : max_frames;
  size_t bytes_to_read = frames_to_read * frame_bytes;

  AudioBuffer result;
  result.spec = spec_;
  result.data.assign(buffer_.begin(), buffer_.begin() + bytes_to_read);

  buffer_.erase(buffer_.begin(), buffer_.begin() + bytes_to_read);
  return result;
}

size_t AudioRecorder::available_frames() const {
  hal::LockGuard lock(mutex_);
  uint32_t frame_bytes = spec_.frame_size_bytes();
  return frame_bytes > 0 ? buffer_.size() / frame_bytes : 0;
}

// AudioPlayer Implementation

Result<void> AudioPlayer::open(const AudioSpec& spec) {
  hal::LockGuard lock(mutex_);
  if (spec.sample_rate == 0 || spec.channels == 0) {
    return Result<void>(Status::Invalid, "Invalid AudioSpec parameters");
  }
  spec_ = spec;
  buffer_.clear();
  state_ = AudioState::Stopped;
  return Ok();
}

Result<void> AudioPlayer::play() {
  hal::LockGuard lock(mutex_);
  if (spec_.sample_rate == 0) {
    return Result<void>(Status::Invalid, "AudioPlayer must be opened before playback");
  }
  state_ = AudioState::Playing;
  return Ok();
}

Result<void> AudioPlayer::pause() {
  hal::LockGuard lock(mutex_);
  if (state_ != AudioState::Playing) {
    return Result<void>(Status::Invalid, "AudioPlayer is not playing");
  }
  state_ = AudioState::Paused;
  return Ok();
}

Result<void> AudioPlayer::stop() {
  hal::LockGuard lock(mutex_);
  state_ = AudioState::Stopped;
  buffer_.clear();
  return Ok();
}

Result<void> AudioPlayer::write_samples(const AudioBuffer& buffer) {
  hal::LockGuard lock(mutex_);
  if (buffer.data.empty()) {
    return Ok();
  }

  size_t start_idx = buffer_.size();
  buffer_.insert(buffer_.end(), buffer.data.begin(), buffer.data.end());

  if (std::abs(volume_ - 1.0f) > 1e-4f && spec_.format == AudioSampleFormat::PCM_S16LE) {
    int16_t* samples = reinterpret_cast<int16_t*>(buffer_.data() + start_idx);
    size_t count = (buffer_.size() - start_idx) / sizeof(int16_t);
    for (size_t i = 0; i < count; ++i) {
      float val = samples[i] * volume_;
      val = std::clamp(val, -32768.0f, 32767.0f);
      samples[i] = static_cast<int16_t>(val);
    }
  }

  return Ok();
}

Result<AudioBuffer> AudioPlayer::get_playback_output(size_t max_frames) {
  hal::LockGuard lock(mutex_);
  uint32_t frame_bytes = spec_.frame_size_bytes();
  if (frame_bytes == 0) {
    return Result<AudioBuffer>(Status::Invalid, "AudioPlayer spec not set");
  }

  size_t available = buffer_.size() / frame_bytes;
  size_t frames_to_read = (max_frames == 0 || max_frames > available) ? available : max_frames;
  size_t bytes_to_read = frames_to_read * frame_bytes;

  AudioBuffer result;
  result.spec = spec_;
  result.data.assign(buffer_.begin(), buffer_.begin() + bytes_to_read);

  buffer_.erase(buffer_.begin(), buffer_.begin() + bytes_to_read);
  return result;
}

size_t AudioPlayer::buffered_frames() const {
  hal::LockGuard lock(mutex_);
  uint32_t frame_bytes = spec_.frame_size_bytes();
  return frame_bytes > 0 ? buffer_.size() / frame_bytes : 0;
}

}  // namespace aios::media
