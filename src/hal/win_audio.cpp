// The microphone and the speaker on Windows: not wired up (no WASAPI capture/
// playback here). An empty endpoint list and a refusal are the honest
// answers a substrate with no implementation gives — never a generated tone,
// which is the exact failure posix_speech.cpp's own header comment rejects.
// speech_voices/synthesize/recognizer/recognize are posix_speech.cpp (shared,
// unchanged on this host); wave_encode/wave_decode are src/media/wave.cpp.
#include "aios/hal/audio.hpp"

namespace aios::hal {

Result<std::vector<AudioEndpoint>> audio_endpoints() { return std::vector<AudioEndpoint>{}; }

Result<std::vector<int16_t>> audio_capture(uint32_t, size_t, uint32_t) {
  return Result<std::vector<int16_t>>(Status::Unsupported, "no capture device on this host");
}

Result<void> audio_play(const int16_t*, size_t, uint32_t) {
  return Result<void>(Status::Unsupported, "no output device on this host");
}

}  // namespace aios::hal
