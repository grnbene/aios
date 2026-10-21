// The microphone and the speaker on a macOS substrate.
//
// Capture goes through CoreAudio's AudioQueue, which is the real input path:
// the callback receives what the hardware converter produced, and nothing is
// generated when the device gives nothing. Playback hands the sound to
// afplay, the substrate's own player.
//
// The machine may be refused a microphone (the substrate asks its owner first).
// That refusal is reported as a refusal. The voice engine above used to push a
// 440Hz sine wave into its own recorder whenever the buffer came up empty.
#include "aios/hal/audio.hpp"
#include "aios/hal/process.hpp"
#include "aios/hal/sync.hpp"

#include <AudioToolbox/AudioToolbox.h>

#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace aios::hal {
namespace {

constexpr int kQueueBuffers = 4;

struct Capture {
  std::vector<int16_t>* out = nullptr;
  size_t wanted = 0;
  bool done = false;
};

void on_input(void* user, AudioQueueRef queue, AudioQueueBufferRef buffer,
              const AudioTimeStamp*, UInt32 packets, const AudioStreamPacketDescription*) {
  auto* cap = static_cast<Capture*>(user);
  if (cap && !cap->done && packets > 0) {
    const int16_t* pcm = static_cast<const int16_t*>(buffer->mAudioData);
    size_t n = buffer->mAudioDataByteSize / sizeof(int16_t);
    for (size_t i = 0; i < n && cap->out->size() < cap->wanted; ++i) cap->out->push_back(pcm[i]);
    if (cap->out->size() >= cap->wanted) cap->done = true;
  }
  AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
}

AudioStreamBasicDescription mono_s16(uint32_t rate) {
  AudioStreamBasicDescription f{};
  f.mSampleRate = rate;
  f.mFormatID = kAudioFormatLinearPCM;
  f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
  f.mFramesPerPacket = 1;
  f.mChannelsPerFrame = 1;
  f.mBitsPerChannel = 16;
  f.mBytesPerFrame = 2;
  f.mBytesPerPacket = 2;
  return f;
}

std::string device_name(AudioObjectID id) {
  CFStringRef name = nullptr;
  UInt32 size = sizeof(name);
  AudioObjectPropertyAddress addr{kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                  kAudioObjectPropertyElementMain};
  if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, &name) != noErr || !name) return {};
  char buf[256] = {0};
  CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8);
  CFRelease(name);
  return buf;
}

uint32_t stream_channels(AudioObjectID id, bool input) {
  AudioObjectPropertyAddress addr{kAudioDevicePropertyStreamConfiguration,
                                  input ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput,
                                  kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(id, &addr, 0, nullptr, &size) != noErr || size == 0) return 0;
  std::vector<uint8_t> raw(size);
  auto* list = reinterpret_cast<AudioBufferList*>(raw.data());
  if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, list) != noErr) return 0;
  uint32_t ch = 0;
  for (UInt32 i = 0; i < list->mNumberBuffers; ++i) ch += list->mBuffers[i].mNumberChannels;
  return ch;
}

}  // namespace

Result<std::vector<AudioEndpoint>> audio_endpoints() {
  AudioObjectPropertyAddress addr{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                  kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr) {
    return Result<std::vector<AudioEndpoint>>(Status::Unsupported, "CoreAudio would not list the devices");
  }
  std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
  if (ids.empty()) return std::vector<AudioEndpoint>{};
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, ids.data()) != noErr) {
    return Result<std::vector<AudioEndpoint>>(Status::Unsupported, "CoreAudio would not list the devices");
  }

  auto default_of = [](bool input) -> AudioObjectID {
    AudioObjectID id = kAudioObjectUnknown;
    UInt32 sz = sizeof(id);
    AudioObjectPropertyAddress a{input ? kAudioHardwarePropertyDefaultInputDevice
                                       : kAudioHardwarePropertyDefaultOutputDevice,
                                 kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, &id);
    return id;
  };
  AudioObjectID def_in = default_of(true), def_out = default_of(false);

  std::vector<AudioEndpoint> out;
  for (AudioObjectID id : ids) {
    std::string name = device_name(id);
    for (bool input : {true, false}) {
      uint32_t ch = stream_channels(id, input);
      if (ch == 0) continue;
      AudioEndpoint e;
      e.id = std::to_string(id);
      e.name = name;
      e.input = input;
      e.channels = ch;
      e.is_default = (input ? def_in : def_out) == id;
      out.push_back(e);
    }
  }
  return out;
}

Result<std::vector<int16_t>> audio_capture(uint32_t sample_rate, size_t frames, uint32_t timeout_ms) {
  if (frames == 0) return std::vector<int16_t>{};
  if (sample_rate == 0) sample_rate = 16000;

  std::vector<int16_t> pcm;
  pcm.reserve(frames);
  Capture cap;
  cap.out = &pcm;
  cap.wanted = frames;

  AudioStreamBasicDescription fmt = mono_s16(sample_rate);
  AudioQueueRef queue = nullptr;
  OSStatus st = AudioQueueNewInput(&fmt, on_input, &cap, nullptr, nullptr, 0, &queue);
  if (st != noErr || !queue) {
    return Result<std::vector<int16_t>>(Status::Unsupported,
        "CoreAudio would not open an input queue (status " + std::to_string((int)st) +
        "); this machine may have no microphone, or may not have been given one");
  }

  UInt32 bytes = (UInt32)((sample_rate / 10) * sizeof(int16_t));   // ~100ms per buffer
  for (int i = 0; i < kQueueBuffers; ++i) {
    AudioQueueBufferRef buf = nullptr;
    if (AudioQueueAllocateBuffer(queue, bytes, &buf) != noErr) break;
    AudioQueueEnqueueBuffer(queue, buf, 0, nullptr);
  }

  st = AudioQueueStart(queue, nullptr);
  if (st != noErr) {
    AudioQueueDispose(queue, true);
    return Result<std::vector<int16_t>>(Status::Denied,
        "CoreAudio refused to start recording (status " + std::to_string((int)st) +
        "); the substrate grants microphone access per program");
  }

  uint32_t budget = timeout_ms ? timeout_ms
                               : (uint32_t)((frames * 1000ull) / sample_rate) + 3000;
  uint32_t waited = 0;
  while (!cap.done && waited < budget) {
    // The queue's callbacks run on a run loop of their own; pumping this
    // thread's lets them land without a thread of our own to join.
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
    waited += 20;
  }
  AudioQueueStop(queue, true);
  AudioQueueDispose(queue, true);

  if (pcm.empty()) {
    return Result<std::vector<int16_t>>(Status::IoError,
        "the input device produced no samples in " + std::to_string(waited) + "ms");
  }
  return pcm;
}

Result<void> audio_play(const int16_t* samples, size_t frames, uint32_t sample_rate) {
  if (!samples || frames == 0) return Ok();
  std::string player = find_program({"afplay"});
  if (player.empty()) return Result<void>(Status::Unsupported, "no afplay on this machine: nothing can play");

  auto wav = scratch_path(".wav");
  if (!wav.ok()) return wav.error();
  auto encoded = wave_encode(samples, frames, sample_rate ? sample_rate : 22050, 1);
  FILE* f = ::fopen(wav->c_str(), "wb");
  if (!f) return Result<void>(Status::IoError, "cannot write the audio to play");
  bool wrote = ::fwrite(encoded.data(), 1, encoded.size(), f) == encoded.size();
  ::fclose(f);
  if (!wrote) { ::unlink(wav->c_str()); return Result<void>(Status::IoError, "short write playing audio"); }

  double seconds = static_cast<double>(frames) / static_cast<double>(sample_rate ? sample_rate : 22050);
  auto run = run_tool({player, *wav}, (uint32_t)(seconds * 1000) + 15000);
  ::unlink(wav->c_str());
  if (!run.ok()) return run.error();
  if (run->exit_code != 0) return Result<void>(Status::IoError, "afplay refused: " + run->output.substr(0, 200));
  return Ok();
}

}  // namespace aios::hal
