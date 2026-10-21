// Speech, on a substrate that has an engine for it.
//
// There is no model in this repository and there is not going to be one: a
// speech synthesizer and a speech recogniser are things the machine HAS, the
// way it has a disk, and this file reaches the ones the substrate provides.
// macOS ships `say`; a Linux box has espeak-ng, or piper, or nothing.
// Recognition is a whisper-family binary if one is installed.
//
// What this file will not do is answer anyway. The engine that used to sit
// above it returned a 220Hz sine wave for every sentence and the eight words
// "complete input speech recognized system ready command task" for every
// recording, and both were indistinguishable from working until somebody
// listened.
#include "aios/hal/audio.hpp"
#include "aios/hal/process.hpp"
#include "aios/hal/host.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#if defined(_WIN32)
#include <io.h>
#ifndef R_OK
#define R_OK 4
#endif
#define access _access
#else
#include <unistd.h>
#endif

namespace aios::hal {
namespace {

constexpr uint32_t kSynthTimeoutMs = 30000;
constexpr uint32_t kRecogniseTimeoutMs = 600000;   // a real model on CPU is slow

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

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

bool write_whole(const std::string& path, const std::vector<uint8_t>& data) {
  FILE* f = ::fopen(path.c_str(), "wb");
  if (!f) return false;
  bool ok = data.empty() || ::fwrite(data.data(), 1, data.size(), f) == data.size();
  ::fclose(f);
  return ok;
}

// ---- synthesis --------------------------------------------------------------

struct Synthesizer {
  std::string program;
  std::string kind;    // "say", "espeak-ng", "piper"
};

Synthesizer find_synthesizer() {
  std::string p = find_program({"say"});
  if (!p.empty()) return {p, "say"};
  p = find_program({"espeak-ng", "espeak"});
  if (!p.empty()) return {p, "espeak-ng"};
  p = find_program({"pico2wave"});
  if (!p.empty()) return {p, "pico2wave"};
  return {};
}

// `say -v '?'` lists the voices the engine really has, one per line:
//   Samantha            en_US    # Hello! My name is Samantha.
std::vector<SpeechVoice> parse_say_voices(const std::string& text) {
  std::vector<SpeechVoice> out;
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    at = end + 1;
    size_t hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    line = trim(line);
    if (line.empty()) continue;
    size_t gap = line.find("  ");
    if (gap == std::string::npos) continue;
    SpeechVoice v;
    v.name = trim(line.substr(0, gap));
    v.language = trim(line.substr(gap));
    v.id = v.name;
    if (!v.name.empty()) out.push_back(v);
  }
  return out;
}

// espeak-ng --voices prints a fixed-column table whose second and fourth
// columns are the language tag and the voice name.
std::vector<SpeechVoice> parse_espeak_voices(const std::string& text) {
  std::vector<SpeechVoice> out;
  size_t at = 0;
  bool first = true;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    at = end + 1;
    if (first) { first = false; continue; }   // the header row
    std::vector<std::string> col;
    size_t i = 0;
    while (i < line.size()) {
      while (i < line.size() && line[i] == ' ') ++i;
      size_t start = i;
      while (i < line.size() && line[i] != ' ') ++i;
      if (i > start) col.push_back(line.substr(start, i - start));
    }
    if (col.size() < 4) continue;
    SpeechVoice v;
    v.language = col[1];
    v.name = col[3];
    v.id = col[3];
    v.gender = col[2] == "M" ? "male" : (col[2] == "F" ? "female" : "");
    out.push_back(v);
  }
  return out;
}

// ---- recognition ------------------------------------------------------------

struct Recogniser {
  std::string program;
  std::string kind;    // "whisper-cli" (whisper.cpp) or "whisper" (python)
  std::string model;   // whisper.cpp needs one named on the command line
};

Recogniser find_recogniser() {
  std::string p = find_program({"whisper-cli", "whisper-cpp", "whisper.cpp"});
  if (!p.empty()) {
    Recogniser r{p, "whisper-cli", {}};
    // whisper.cpp will not run without a model file; look where the common
    // packagings put one rather than guessing at a name.
    for (const char* candidate : {
             "/usr/local/share/whisper-cpp/ggml-base.en.bin",
             "/opt/homebrew/share/whisper-cpp/ggml-base.en.bin",
             "/usr/share/whisper.cpp/models/ggml-base.en.bin",
             "/usr/local/share/whisper.cpp/ggml-base.en.bin"}) {
      if (::access(candidate, R_OK) == 0) { r.model = candidate; break; }
    }
    return r;
  }
  p = find_program({"whisper"});
  if (!p.empty()) return {p, "whisper", {}};
  return {};
}

// whisper.cpp writes "[00:00:00.000 --> 00:00:02.000]   the text" per segment.
// The python whisper CLI writes the same shape. Either way the transcript is
// what follows the bracket.
SpeechTranscript parse_whisper(const std::string& text) {
  SpeechTranscript t;
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    at = end + 1;
    size_t close = line.find(']');
    if (line.empty() || line[0] != '[' || close == std::string::npos) continue;
    std::string stamp = line.substr(1, close - 1);
    std::string said = trim(line.substr(close + 1));
    if (said.empty()) continue;

    SpeechWord w;
    w.text = said;
    // "00:00:00.000 --> 00:00:02.000" — take both ends as milliseconds.
    auto to_ms = [](const std::string& hms) -> uint64_t {
      unsigned h = 0, m = 0, s = 0, ms = 0;
      if (std::sscanf(hms.c_str(), "%u:%u:%u.%u", &h, &m, &s, &ms) == 4)
        return ((uint64_t)h * 3600 + m * 60 + s) * 1000 + ms;
      if (std::sscanf(hms.c_str(), "%u:%u.%u", &m, &s, &ms) == 3)
        return ((uint64_t)m * 60 + s) * 1000 + ms;
      return 0;
    };
    size_t arrow = stamp.find("-->");
    if (arrow != std::string::npos) {
      w.start_ms = to_ms(trim(stamp.substr(0, arrow)));
      w.end_ms = to_ms(trim(stamp.substr(arrow + 3)));
    }
    t.words.push_back(w);
    if (!t.text.empty()) t.text += " ";
    t.text += said;
  }
  return t;
}

}  // namespace

Result<std::vector<SpeechVoice>> speech_voices() {
  Synthesizer eng = find_synthesizer();
  if (eng.program.empty()) {
    return Result<std::vector<SpeechVoice>>(Status::Unsupported,
        "this machine has no speech synthesizer (looked for say, espeak-ng, pico2wave)");
  }
  if (eng.kind == "say") {
    auto r = run_tool({eng.program, "-v", "?"}, 10000);
    if (!r.ok()) return r.error();
    return parse_say_voices(r->output);
  }
  if (eng.kind == "espeak-ng") {
    auto r = run_tool({eng.program, "--voices"}, 10000);
    if (!r.ok()) return r.error();
    return parse_espeak_voices(r->output);
  }
  return std::vector<SpeechVoice>{SpeechVoice{"default", "pico2wave", "en-US", ""}};
}

Result<SpeechAudio> speech_synthesize(const std::string& text, const std::string& voice_id,
                                      uint32_t words_per_minute, uint32_t sample_rate) {
  if (text.empty()) return Result<SpeechAudio>(Status::Invalid, "nothing to say");
  Synthesizer eng = find_synthesizer();
  if (eng.program.empty()) {
    return Result<SpeechAudio>(Status::Unsupported,
        "this machine has no speech synthesizer (looked for say, espeak-ng, pico2wave)");
  }
  if (sample_rate == 0) sample_rate = 22050;

  auto wav = scratch_path(".wav");
  if (!wav.ok()) return wav.error();

  std::vector<std::string> argv;
  if (eng.kind == "say") {
    argv = {eng.program};
    if (!voice_id.empty() && voice_id != "default") { argv.push_back("-v"); argv.push_back(voice_id); }
    if (words_per_minute) { argv.push_back("-r"); argv.push_back(std::to_string(words_per_minute)); }
    argv.push_back("--data-format=LEI16@" + std::to_string(sample_rate));
    argv.push_back("--file-format=WAVE");
    argv.push_back("-o");
    argv.push_back(*wav);
    argv.push_back(text);
  } else if (eng.kind == "espeak-ng") {
    argv = {eng.program, "-w", *wav};
    if (!voice_id.empty() && voice_id != "default") { argv.push_back("-v"); argv.push_back(voice_id); }
    if (words_per_minute) { argv.push_back("-s"); argv.push_back(std::to_string(words_per_minute)); }
    argv.push_back(text);
  } else {
    argv = {eng.program, "-w", *wav, text};
  }

  auto run = run_tool(argv, kSynthTimeoutMs);
  if (!run.ok()) { ::unlink(wav->c_str()); return run.error(); }
  if (run->exit_code != 0) {
    ::unlink(wav->c_str());
    return Result<SpeechAudio>(Status::IoError,
        eng.kind + " refused: " + trim(run->output).substr(0, 200));
  }

  auto bytes = read_whole(*wav);
  ::unlink(wav->c_str());
  if (!bytes.ok()) return bytes.error();
  auto decoded = wave_decode(bytes->data(), bytes->size());
  if (!decoded.ok()) return decoded.error();

  SpeechAudio out;
  out.pcm = std::move(decoded->pcm);
  out.sample_rate = decoded->sample_rate;
  out.engine = eng.kind;
  if (out.pcm.empty()) {
    return Result<SpeechAudio>(Status::IoError, eng.kind + " produced no audio for that text");
  }
  return out;
}

Result<std::string> speech_recognizer() {
  Recogniser eng = find_recogniser();
  if (eng.program.empty()) {
    return Result<std::string>(Status::Unsupported,
        "no speech recogniser installed (looked for whisper-cli, whisper-cpp, whisper)");
  }
  if (eng.kind == "whisper-cli" && eng.model.empty()) {
    return Result<std::string>(Status::Unsupported,
        "whisper.cpp is installed at " + eng.program + " but no model file was found to run it with");
  }
  return eng.kind + " (" + eng.program + ")";
}

Result<SpeechTranscript> speech_recognize(const int16_t* samples, size_t frames, uint32_t sample_rate,
                                          const std::string& language) {
  if (!samples || frames == 0) return Result<SpeechTranscript>(Status::Invalid, "no audio to recognise");
  Recogniser eng = find_recogniser();
  if (eng.program.empty()) {
    return Result<SpeechTranscript>(Status::Unsupported,
        "this machine has no speech recogniser installed (looked for whisper-cli, whisper-cpp, whisper)");
  }
  if (eng.kind == "whisper-cli" && eng.model.empty()) {
    return Result<SpeechTranscript>(Status::Unsupported,
        "whisper.cpp is installed but no model file was found to run it with");
  }

  // Every recogniser worth the name reads 16 kHz mono; hand it exactly that.
  auto wav = scratch_path(".wav");
  if (!wav.ok()) return wav.error();
  auto encoded = wave_encode(samples, frames, sample_rate ? sample_rate : 16000, 1);
  if (!write_whole(*wav, encoded)) {
    return Result<SpeechTranscript>(Status::IoError, "cannot write the audio for the recogniser");
  }

  std::vector<std::string> argv;
  if (eng.kind == "whisper-cli") {
    argv = {eng.program, "-m", eng.model, "-f", *wav, "-nt"};
    if (!language.empty()) { argv.push_back("-l"); argv.push_back(language); }
  } else {
    argv = {eng.program, *wav, "--output_format", "txt", "--fp16", "False"};
    if (!language.empty()) { argv.push_back("--language"); argv.push_back(language); }
  }

  auto run = run_tool(argv, kRecogniseTimeoutMs);
  ::unlink(wav->c_str());
  if (!run.ok()) return run.error();
  if (run->exit_code != 0) {
    return Result<SpeechTranscript>(Status::IoError,
        eng.kind + " refused: " + trim(run->output).substr(0, 300));
  }

  SpeechTranscript t = parse_whisper(run->output);
  if (t.text.empty()) {
    // No timestamped lines: some builds print the bare transcript. Take the
    // last non-empty line rather than nothing, but never invent one.
    std::string last;
    size_t at = 0;
    while (at < run->output.size()) {
      size_t end = run->output.find('\n', at);
      if (end == std::string::npos) end = run->output.size();
      std::string line = trim(run->output.substr(at, end - at));
      at = end + 1;
      if (!line.empty() && line[0] != '[' && line.find("whisper_") == std::string::npos) last = line;
    }
    t.text = last;
  }
  t.engine = eng.kind;
  t.language = language;
  // Whisper reports no per-word probability on the command line, so none is
  // claimed. A confidence invented here would be believed.
  t.confidence = 0.0f;
  return t;
}

}  // namespace aios::hal
