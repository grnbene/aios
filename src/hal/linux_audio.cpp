// The microphone and the speaker on a Linux substrate, through ALSA's own
// tools. arecord and aplay ARE the substrate's audio path — the same one every
// other program on the machine records and plays through — so what this
// captures is what the hardware heard.
//
// It refuses when there is no capture device rather than producing something.
// The voice engine above used to push a 440Hz sine wave into its own recorder
// whenever the buffer was empty and then "transcribe" it.
#include "aios/hal/audio.hpp"
#include "aios/hal/process.hpp"

#include <cstdio>
#include <unistd.h>

namespace aios::hal {
namespace {

Result<std::vector<uint8_t>> read_whole(const std::string& path) {
  FILE* f = ::fopen(path.c_str(), "rb");
  if (!f) return Result<std::vector<uint8_t>>(Status::IoError, "cannot read " + path);
  std::vector<uint8_t> out;
  uint8_t chunk[8192];
  size_t n;
  while ((n = ::fread(chunk, 1, sizeof chunk, f)) > 0) out.insert(out.end(), chunk, chunk + n);
  ::fclose(f);
  return out;
}

// `arecord -l` / `aplay -l` list what the kernel has actually bound:
//   card 0: PCH [HDA Intel PCH], device 0: ALC3204 Analog [ALC3204 Analog]
std::vector<AudioEndpoint> parse_alsa_list(const std::string& text, bool input) {
  std::vector<AudioEndpoint> out;
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    at = end + 1;
    if (line.rfind("card ", 0) != 0) continue;
    unsigned card = 0, dev = 0;
    char cname[128] = {0}, dname[128] = {0};
    if (std::sscanf(line.c_str(), "card %u: %127[^[] [%*[^]]], device %u: %127[^[]", &card, cname, &dev, dname) < 3) {
      continue;
    }
    AudioEndpoint e;
    e.id = "hw:" + std::to_string(card) + "," + std::to_string(dev);
    std::string name(cname);
    while (!name.empty() && name.back() == ' ') name.pop_back();
    std::string d(dname);
    while (!d.empty() && d.back() == ' ') d.pop_back();
    e.name = name + (d.empty() ? "" : " / " + d);
    e.input = input;
    e.is_default = out.empty();
    out.push_back(e);
  }
  return out;
}

}  // namespace

Result<std::vector<AudioEndpoint>> audio_endpoints() {
  std::vector<AudioEndpoint> out;
  std::string rec = find_program({"arecord"});
  std::string play = find_program({"aplay"});
  if (rec.empty() && play.empty()) {
    return Result<std::vector<AudioEndpoint>>(Status::Unsupported,
        "this machine has no ALSA tools (looked for arecord, aplay)");
  }
  if (!rec.empty()) {
    auto r = run_tool({rec, "-l"}, 5000);
    if (r.ok()) for (auto& e : parse_alsa_list(r->output, true)) out.push_back(e);
  }
  if (!play.empty()) {
    auto r = run_tool({play, "-l"}, 5000);
    if (r.ok()) for (auto& e : parse_alsa_list(r->output, false)) out.push_back(e);
  }
  return out;
}

Result<std::vector<int16_t>> audio_capture(uint32_t sample_rate, size_t frames, uint32_t timeout_ms) {
  std::string rec = find_program({"arecord"});
  if (rec.empty()) {
    return Result<std::vector<int16_t>>(Status::Unsupported, "no arecord on this machine: nothing can record");
  }
  if (sample_rate == 0) sample_rate = 16000;
  if (frames == 0) return std::vector<int16_t>{};

  double seconds = static_cast<double>(frames) / static_cast<double>(sample_rate);
  auto wav = scratch_path(".wav");
  if (!wav.ok()) return wav.error();

  char dur[32];
  std::snprintf(dur, sizeof dur, "%.3f", seconds);
  auto run = run_tool({rec, "-q", "-f", "S16_LE", "-c", "1",
                       "-r", std::to_string(sample_rate), "-d", dur, "-t", "wav", *wav},
                      timeout_ms ? timeout_ms : (uint32_t)(seconds * 1000) + 10000);
  if (!run.ok()) { ::unlink(wav->c_str()); return run.error(); }
  if (run->exit_code != 0) {
    std::string why = run->output.substr(0, 200);
    ::unlink(wav->c_str());
    return Result<std::vector<int16_t>>(Status::IoError, "arecord refused: " + why);
  }

  auto bytes = read_whole(*wav);
  ::unlink(wav->c_str());
  if (!bytes.ok()) return bytes.error();
  auto decoded = wave_decode(bytes->data(), bytes->size());
  if (!decoded.ok()) return decoded.error();
  return decoded->pcm;
}

Result<void> audio_play(const int16_t* samples, size_t frames, uint32_t sample_rate) {
  if (!samples || frames == 0) return Ok();
  std::string play = find_program({"aplay"});
  if (play.empty()) {
    return Result<void>(Status::Unsupported, "no aplay on this machine: nothing can play");
  }
  auto wav = scratch_path(".wav");
  if (!wav.ok()) return wav.error();
  auto encoded = wave_encode(samples, frames, sample_rate ? sample_rate : 22050, 1);
  FILE* f = ::fopen(wav->c_str(), "wb");
  if (!f) return Result<void>(Status::IoError, "cannot write the audio to play");
  bool wrote = ::fwrite(encoded.data(), 1, encoded.size(), f) == encoded.size();
  ::fclose(f);
  if (!wrote) { ::unlink(wav->c_str()); return Result<void>(Status::IoError, "short write playing audio"); }

  double seconds = static_cast<double>(frames) / static_cast<double>(sample_rate ? sample_rate : 22050);
  auto run = run_tool({play, "-q", *wav}, (uint32_t)(seconds * 1000) + 15000);
  ::unlink(wav->c_str());
  if (!run.ok()) return run.error();
  if (run->exit_code != 0) return Result<void>(Status::IoError, "aplay refused: " + run->output.substr(0, 200));
  return Ok();
}

}  // namespace aios::hal
