// `organ` — a primitive this machine grows for itself.
//
// A dream that needs something the kernel cannot do is not refused: the mind
// writes the thing in C++, the machine builds it here, loads it with the
// substrate's dynamic loader and keeps it under its root. The next dream has
// it. The machine's vocabulary grows by accretion, not by release.
//
// The ABI is one function, text in and text out, so an organ is written
// without knowing anything about this kernel:
//     extern "C" const char* aios_organ(const char* request);
#include "cli.hpp"
#include "aios/userland/organs.hpp"
#include <algorithm>

namespace aios::cli {

namespace {

template <typename T> T need(Result<T> r) { if (!r.ok()) throw Fail{r.error()}; return std::move(*r); }
void need(Result<void> r) { if (!r.ok()) throw Fail{r.error()}; }

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool valid_name(const std::string& n) {
  if (n.empty() || n.size() > 40) return false;
  for (char c : n)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  return true;
}

// The C++ out of the mind's reply: a fenced block if there is one, otherwise
// from the first #include to the end.
std::string source_of(const std::string& reply) {
  size_t fence = reply.find("```");
  if (fence != std::string::npos) {
    size_t nl = reply.find('\n', fence);
    if (nl == std::string::npos) return {};
    size_t end = reply.find("```", nl + 1);
    return trim(reply.substr(nl + 1, end == std::string::npos ? std::string::npos : end - nl - 1));
  }
  size_t inc = reply.find("#include");
  if (inc == std::string::npos) return {};
  return trim(reply.substr(inc));
}

std::string contract(const std::string& name, const std::string& what) {
  return
    "Grow an organ for this machine. An organ is ONE C++20 source file the machine compiles into a shared library and loads.\n"
    "Name: " + name + "\nWhat it must do: " + what + "\n\n"
    "Rules:\n"
    "- Export exactly:  extern \"C\" const char* aios_organ(const char* request);\n"
    "- `request` is text (often JSON). Return text (often JSON). The returned pointer must stay valid until the next call: build the answer in a `static std::string out;` and return out.c_str().\n"
    "- The request \"describe\" must return one line saying what this organ does and what requests it takes.\n"
    "- An organ that DRAWS may also export:  extern \"C\" void aios_paint(uint32_t* buf, int w, int h, double t);\n"
    "  buf is w*h pixels of 0xAARRGGBB, row-major, yours to fill; t is seconds. This is for what the face language cannot reach — a field of noise, a particle system, a million pixels of arithmetic. Optional: leave it out unless the organ is meant to draw.\n"
    "- Standard library only (<string> <vector> <cmath> <sstream> <map> <algorithm> <cstdio> <cstring> <random> <chrono>…). No third-party headers, no network, no files unless the request names a path.\n"
    "- Never crash on a bad request: answer with text starting `error:`.\n"
    "- Self-contained, deterministic where it can be, fast.\n"
    "Reply with:\n    answer:\n    ```cpp\n    <the whole file>\n    ```\n";
}

std::string read_text(Kernel& k, const std::string& path) {
  auto f = k.host().read_file(path);
  if (!f.ok()) return {};
  return std::string((const char*)f->data(), f->size());
}

}  // namespace

Result<void> organ_grow(Session& s, const std::string& name, const std::string& what, std::string* report) {
  if (!s.kernel || !s.me) return Result<void>(Status::Invalid, "no kernel");
  if (!valid_name(name)) return Result<void>(Status::Invalid, "an organ's name is lower-case letters, digits, - and _");
  if (!s.host) return Result<void>(Status::Unsupported, "no host tool bound to this session: an organ is built with the substrate's compiler");
  Kernel& k = *s.kernel;
  Out& o = *s.out;
  const std::string root = k.config().root;
  const std::string dir = userland::organ_dir(root, name);
  const std::string cpp = dir + "/" + name + ".cpp";
  const std::string so = userland::organ_library_path(root, name);
  const std::string log = dir + "/build.log";
  (void)k.host().make_dir(root + "/organs");
  (void)k.host().make_dir(dir);

  o << o.style(Bold()) << "organ " << name << o.style(Reset()) << " — growing: " << what << "\n";
  o.flush();

  std::string correction;
  std::string built_report;
  for (int round = 0; round < 3; ++round) {
    std::string reply;
    AIOS_TRY(ask(s, contract(name, what) + correction, "mind", 3, &reply, /*remember=*/false));
    const std::string src = source_of(reply);
    if (src.empty()) {
      correction = "\nYour previous reply held no C++ source. Reply with the whole file in a ```cpp fence.\n";
      o << "  the mind's reply held no source; asking again\n";
      continue;
    }
    AIOS_TRY(k.host().write_file(cpp, ByteSpan((const uint8_t*)src.data(), src.size())));
    o << "  wrote " << cpp << " (" << src.size() << " bytes); building\n";
    o.flush();
    // The compiler's own words go to a file so they can be read back — the
    // host tool's child inherits the console's streams, and a build error
    // that only scrolls past is one the mind never sees.
    const std::string sh = "c++ -std=c++20 -O2 -shared -fPIC -o '" + so + "' '" + cpp + "' > '" + log + "' 2>&1";
    auto built = s.host({"sh", "-c", sh}, o, *s.me);
    const std::string words = read_text(k, log);
    if (!built.ok()) {
      std::string tail = words.size() > 4000 ? words.substr(words.size() - 4000) : words;
      o << o.style(Red()) << "  build failed" << o.style(Reset()) << "\n";
      for (auto& line : split(tail, '\n')) if (!line.empty()) o << "    │ " << line << "\n";
      o.flush();
      correction = "\nThe machine compiled your previous file and the compiler said:\n" + tail +
                   "\nFix it and reply with the WHOLE corrected file in a ```cpp fence.\nYour previous file was:\n```cpp\n" + src + "\n```\n";
      continue;
    }
    userland::organ_forget(name);
    auto desc = userland::organ_call(root, name, "describe");
    if (!desc.ok()) {
      correction = "\nThe machine built your file but could not use it: " + desc.error().message +
                   "\nReply with the WHOLE corrected file in a ```cpp fence.\n";
      o << "  built, but: " << desc.error().message << "\n";
      continue;
    }
    built_report = *desc;
    o << o.style(Green()) << "  built and loaded" << o.style(Reset()) << ": " << *desc << "\n";
    if (!words.empty()) {
      int warnings = 0;
      for (auto& line : split(words, '\n')) if (line.find("warning:") != std::string::npos) ++warnings;
      if (warnings) o << "  (" << warnings << " compiler warning(s) in " << log << ")\n";
    }
    StoredObject rec;
    rec.payload = ByteBuffer(built_report.begin(), built_report.end());
    rec.embedding = embed_text("organ " + name + " " + what + " " + built_report);
    rec.metadata.emplace_back("kind", "organ");
    rec.metadata.emplace_back("organ", name);
    rec.metadata.emplace_back("what", what);
    rec.metadata.emplace_back("library", so);
    (void)s.me->store(std::move(rec));
    if (report) *report = built_report;
    return Ok();
  }
  // Three rounds of the compiler's own words went back to the mind and it still
  // could not produce a file this machine builds. Keep that: an organ the
  // machine repeatedly fails to grow is a thing it cannot do yet, and the list
  // of those is worth as much as the list of organs it has.
  record_fault(s, "organ.compile", "could not build an organ for: " + what, name);
  return Result<void>(Status::Exhausted, "organ " + name + ": the mind could not produce a file this machine can build in three rounds");
}

Result<void> organ_command(Session& s, const std::vector<std::string>& a) {
  if (!s.kernel || !s.me) return Result<void>(Status::Invalid, "no kernel");
  Kernel& k = *s.kernel;
  Out& o = *s.out;
  const std::string sub = a.size() > 1 ? a[1] : "list";
  const std::string usage = "organ list | organ grow <name> <what it must do…> | organ call <name> <request…> | organ source <name> | organ remove <name>";
  try {
    if (sub == "list") {
      std::map<std::string, StoredObject> newest;
      for (const auto& id : k.store().find_meta("kind", "organ")) {
        auto ob = k.store().get(id);
        if (!ob.ok()) continue;
        const std::string n = ob->meta("organ");
        auto it = newest.find(n);
        if (it == newest.end() || it->second.timestamp_ns <= ob->timestamp_ns) newest[n] = *ob;
      }
      if (newest.empty()) { o << "no organs grown yet — `organ grow <name> <what it must do>`\n"; return Ok(); }
      o << o.style(Header()) << "Organs this machine has grown" << o.style(Reset()) << "\n";
      for (const auto& [n, ob] : newest) {
        if (ob.meta("removed") == "yes") continue;
        const bool present = k.host().exists(userland::organ_library_path(k.config().root, n));
        o << "  " << o.style(Bold()) << n << o.style(Reset()) << (present ? "" : "  (library missing — grow it again)") << "\n";
        o << "      " << ob.meta("what") << "\n";
        o << "      " << o.style(Dim()) << std::string(ob.payload.begin(), ob.payload.end()) << o.style(Reset()) << "\n";
      }
      o << "a surface calls one with aos.organ(name, request); `organ call <name> <request>` from here\n";
      return Ok();
    }
    if (sub == "grow" || sub == "make") {
      if (a.size() < 4) fail(Status::Invalid, "organ grow <name> <what it must do…>");
      std::string what;
      for (size_t i = 3; i < a.size(); ++i) { if (!what.empty()) what += " "; what += a[i]; }
      need(organ_grow(s, a[2], what));
      return Ok();
    }
    if (sub == "call") {
      if (a.size() < 3) fail(Status::Invalid, "organ call <name> <request…>");
      std::string req;
      for (size_t i = 3; i < a.size(); ++i) { if (!req.empty()) req += " "; req += a[i]; }
      auto r = need(userland::organ_call(k.config().root, a[2], req));
      o << r << (r.empty() || r.back() == '\n' ? "" : "\n");
      return Ok();
    }
    if (sub == "source") {
      if (a.size() < 3) fail(Status::Invalid, "organ source <name>");
      const std::string src = read_text(k, userland::organ_dir(k.config().root, a[2]) + "/" + a[2] + ".cpp");
      if (src.empty()) fail(Status::NotFound, "no source for organ " + a[2]);
      o << src << (src.back() == '\n' ? "" : "\n");
      return Ok();
    }
    if (sub == "remove") {
      if (a.size() < 3) fail(Status::Invalid, "organ remove <name>");
      userland::organ_forget(a[2]);
      (void)k.host().remove(userland::organ_library_path(k.config().root, a[2]));
      StoredObject rec;
      rec.metadata.emplace_back("kind", "organ");
      rec.metadata.emplace_back("organ", a[2]);
      rec.metadata.emplace_back("removed", "yes");
      (void)s.me->store(std::move(rec));
      o << "organ " << a[2] << " removed (its source stays under the root)\n";
      return Ok();
    }
    fail(Status::Invalid, usage);
  } catch (const Fail& f) {
    return Result<void>(f.e.status, f.e.message);
  }
  return Ok();
}

}  // namespace aios::cli
