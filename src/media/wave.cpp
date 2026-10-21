// RIFF/WAVE. This is format code, not host code — chunk walking and byte
// order and nothing else — so it lives with the media subsystem and is
// compiled into every build, including the bare-metal one whose command
// surface reads and writes these files.
//
// It was in src/hal/ at first, which the bare-metal build excludes wholesale,
// and the kernel would not link. Written as a parser rather than a fixed 44-byte header read, because
// real encoders put chunks in the way: macOS `say` emits a JUNK chunk before
// `fmt `, and a reader that assumes the offsets gets silence and blames the
// microphone.
#include "aios/hal/audio.hpp"

#include <cstring>

namespace aios::hal {
namespace {

uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint16_t le16(const uint8_t* p) { return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)); }

void put32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back((uint8_t)(v & 0xFF));
  out.push_back((uint8_t)((v >> 8) & 0xFF));
  out.push_back((uint8_t)((v >> 16) & 0xFF));
  out.push_back((uint8_t)((v >> 24) & 0xFF));
}
void put16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back((uint8_t)(v & 0xFF));
  out.push_back((uint8_t)((v >> 8) & 0xFF));
}
void tag(std::vector<uint8_t>& out, const char* s) { for (int i = 0; i < 4; ++i) out.push_back((uint8_t)s[i]); }

}  // namespace

std::vector<uint8_t> wave_encode(const int16_t* samples, size_t frames, uint32_t sample_rate, uint16_t channels) {
  if (channels == 0) channels = 1;
  uint32_t data_bytes = (uint32_t)(frames * channels * sizeof(int16_t));
  std::vector<uint8_t> out;
  out.reserve(44 + data_bytes);
  tag(out, "RIFF");
  put32(out, 36 + data_bytes);
  tag(out, "WAVE");
  tag(out, "fmt ");
  put32(out, 16);
  put16(out, 1);                                       // PCM
  put16(out, channels);
  put32(out, sample_rate);
  put32(out, sample_rate * channels * sizeof(int16_t)); // byte rate
  put16(out, (uint16_t)(channels * sizeof(int16_t)));   // block align
  put16(out, 16);                                      // bits per sample
  tag(out, "data");
  put32(out, data_bytes);
  if (samples && data_bytes) {
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(samples);
    out.insert(out.end(), raw, raw + data_bytes);
  }
  return out;
}

Result<WaveAudio> wave_decode(const uint8_t* data, size_t size) {
  if (!data || size < 12) return Result<WaveAudio>(Status::Corrupt, "not a RIFF file: too short");
  if (std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
    return Result<WaveAudio>(Status::Corrupt, "not a RIFF/WAVE file");
  }

  WaveAudio out;
  uint16_t format = 0, bits = 0;
  const uint8_t* pcm = nullptr;
  size_t pcm_bytes = 0;

  size_t at = 12;
  while (at + 8 <= size) {
    const uint8_t* id = data + at;
    uint32_t len = le32(data + at + 4);
    size_t body = at + 8;
    if (body + len > size) len = (uint32_t)(size - body);   // truncated tail: take what is there
    if (std::memcmp(id, "fmt ", 4) == 0 && len >= 16) {
      format = le16(data + body);
      out.channels = le16(data + body + 2);
      out.sample_rate = le32(data + body + 4);
      bits = le16(data + body + 14);
    } else if (std::memcmp(id, "data", 4) == 0) {
      pcm = data + body;
      pcm_bytes = len;
    }
    at = body + len + (len & 1);   // chunks are word aligned
  }

  if (out.sample_rate == 0) return Result<WaveAudio>(Status::Corrupt, "WAVE file has no format chunk");
  if (!pcm) return Result<WaveAudio>(Status::Corrupt, "WAVE file has no data chunk");
  if (format != 1 || bits != 16) {
    return Result<WaveAudio>(Status::Unsupported,
        "WAVE is format " + std::to_string(format) + " at " + std::to_string(bits) +
        " bits; this boundary carries 16-bit PCM");
  }
  if (out.channels == 0) out.channels = 1;

  size_t total = pcm_bytes / sizeof(int16_t);
  out.pcm.resize(total);
  if (total) std::memcpy(out.pcm.data(), pcm, total * sizeof(int16_t));

  // Everything above this boundary works in mono; fold the channels here so
  // each substrate does not have to.
  if (out.channels > 1) {
    size_t frames = total / out.channels;
    std::vector<int16_t> mono(frames);
    for (size_t f = 0; f < frames; ++f) {
      int32_t sum = 0;
      for (uint16_t ch = 0; ch < out.channels; ++ch) sum += out.pcm[f * out.channels + ch];
      mono[f] = (int16_t)(sum / (int32_t)out.channels);
    }
    out.pcm = std::move(mono);
    out.channels = 1;
  }
  return out;
}

}  // namespace aios::hal
