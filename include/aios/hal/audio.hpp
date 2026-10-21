#pragma once
// Sound is hardware. A microphone, a speaker, a speech synthesizer and a
// speech recogniser are all things the substrate either has or does not, and
// the media subsystem above this boundary must be told which — never handed a
// generated tone or a canned sentence, because a caller cannot tell those from
// the real thing until a customer does.
#include <cstdint>
#include <string>
#include <vector>
#include "aios/core/result.hpp"

namespace aios::hal {

struct AudioEndpoint {
  std::string id;
  std::string name;
  bool input = false;
  bool is_default = false;
  uint32_t channels = 0;
  uint32_t sample_rate = 0;
};

// What this machine can actually record from and play to. An empty list is an
// answer: there is no sound hardware here.
Result<std::vector<AudioEndpoint>> audio_endpoints();

// Mono 16-bit PCM from the substrate's default input, for as long as asked.
// Refused when there is no input device, or when the substrate will not let
// this process have one.
Result<std::vector<int16_t>> audio_capture(uint32_t sample_rate, size_t frames, uint32_t timeout_ms);

// Mono 16-bit PCM to the substrate's default output. Returns once the sound
// has been handed to the device, not once the listener has heard it.
Result<void> audio_play(const int16_t* samples, size_t frames, uint32_t sample_rate);

struct SpeechVoice {
  std::string id;
  std::string name;
  std::string language;
  std::string gender;   // empty where the engine does not say
};

// The voices the substrate's synthesizer really has.
Result<std::vector<SpeechVoice>> speech_voices();

struct SpeechAudio {
  std::vector<int16_t> pcm;      // mono
  uint32_t sample_rate = 0;
  std::string engine;            // which synthesizer produced it
};

// Speech, synthesised by the substrate's engine from this text in this voice.
// `words_per_minute` of zero means the engine's own default.
Result<SpeechAudio> speech_synthesize(const std::string& text, const std::string& voice_id,
                                      uint32_t words_per_minute, uint32_t sample_rate);

struct SpeechWord {
  std::string text;
  uint64_t start_ms = 0;
  uint64_t end_ms = 0;
  float confidence = 0.0f;
};

struct SpeechTranscript {
  std::string text;
  float confidence = 0.0f;
  std::vector<SpeechWord> words;
  std::string engine;            // which recogniser produced it
  std::string language;
};

// Which recogniser this machine has, if any. Asked separately because a
// caller reporting what the machine CAN do must not have to hand it audio to
// find out.
Result<std::string> speech_recognizer();

// What was said, according to the substrate's recogniser. Refused when the
// machine has no recogniser installed: a transcript nobody spoke is worse than
// no transcript.
Result<SpeechTranscript> speech_recognize(const int16_t* samples, size_t frames, uint32_t sample_rate,
                                          const std::string& language);

// RIFF/WAVE, because that is the format every speech tool on every substrate
// reads and writes. Both directions are here so the substrate implementations
// share one parser instead of each growing their own.
std::vector<uint8_t> wave_encode(const int16_t* samples, size_t frames, uint32_t sample_rate, uint16_t channels = 1);
struct WaveAudio { std::vector<int16_t> pcm; uint32_t sample_rate = 0; uint16_t channels = 1; };
Result<WaveAudio> wave_decode(const uint8_t* data, size_t size);

}  // namespace aios::hal
