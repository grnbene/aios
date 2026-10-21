#include "aios/userland/organs.hpp"
#include <cstdint>
#include "aios/hal/host.hpp"

#include <map>

namespace aios::userland {

std::string organ_dir(const std::string& root, const std::string& name) { return root + "/organs/" + name; }
std::string organ_library_path(const std::string& root, const std::string& name) {
  return organ_dir(root, name) + "/" + name + ".so";
}

namespace {

using OrganFn = const char* (*)(const char*);
using OrganPaintFn = void (*)(uint32_t*, int, int, double);

struct Loaded {
  void* handle = nullptr;
  OrganFn fn = nullptr;
};

std::map<std::string, Loaded>& registry() {
  static std::map<std::string, Loaded> r;
  return r;
}

}  // namespace

Result<std::string> organ_call(const std::string& root, const std::string& name, const std::string& request) {
#if defined(AIOS_METAL)
  (void)root; (void)request;
  return Result<std::string>(Status::Unsupported, "organ " + name + ": this substrate has no dynamic loader yet");
#else
  for (char c : name)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
      return Result<std::string>(Status::Invalid, "an organ's name is lower-case letters, digits, - and _");
  auto& reg = registry();
  auto it = reg.find(name);
  if (it == reg.end()) {
    const std::string path = organ_library_path(root, name);
    void* h = hal::load_library(path);
    if (!h) return Result<std::string>(Status::NotFound, "no organ '" + name + "' is built on this machine (" + path + ": " + hal::library_error() + ")");
    auto fn = reinterpret_cast<OrganFn>(hal::library_symbol(h, "aios_organ"));
    if (!fn) {
      hal::unload_library(h);
      return Result<std::string>(Status::Invalid, path + " does not export aios_organ");
    }
    it = reg.emplace(name, Loaded{h, fn}).first;
  }
  const char* out = it->second.fn(request.c_str());
  return std::string(out ? out : "");
#endif
}

// An organ that PAINTS. aios_organ answers with text, which is the right shape
// for a solver or a codec and the wrong one for a background: a drawing has to
// land as pixels, not as a string describing pixels. The face language's paint
// verbs are evaluated per shape per frame, so a field of noise or a particle
// system is out of their reach — and compiling one of these takes 0.35s against
// the ten to sixty seconds the mind spends writing it, so the native path costs
// nothing worth counting.
//
// Optional: an organ without this symbol is still a perfectly good organ.
Result<void> organ_paint(const std::string& root, const std::string& name,
                         uint32_t* buf, int w, int h, double t) {
#if defined(AIOS_METAL)
  (void)root; (void)buf; (void)w; (void)h; (void)t;
  return Result<void>(Status::Unsupported, "organ " + name + ": this substrate has no dynamic loader yet");
#else
  if (!buf || w <= 0 || h <= 0) return Result<void>(Status::Invalid, "organ " + name + ": nothing to paint into");
  // Loaded through organ_call so one code path owns the registry, the name
  // check and the error text. "describe" is the request every organ answers.
  auto up = organ_call(root, name, "describe");
  if (!up.ok()) return Result<void>(up.error());
  auto& reg = registry();
  auto it = reg.find(name);
  if (it == reg.end()) return Result<void>(Status::NotFound, "organ " + name + " did not stay loaded");
  auto fn = reinterpret_cast<OrganPaintFn>(hal::library_symbol(it->second.handle, "aios_paint"));
  if (!fn) return Result<void>(Status::Unsupported, "organ " + name + " does not export aios_paint, so it cannot draw");
  fn(buf, w, h, t);
  return Ok();
#endif
}

void organ_forget(const std::string& name) {
#if !defined(AIOS_METAL)
  auto& reg = registry();
  auto it = reg.find(name);
  if (it == reg.end()) return;
  // Not unloaded: a page may still hold a pointer into it. Forgotten here, the
  // next call loads the rebuilt library under a fresh handle.
  reg.erase(it);
#else
  (void)name;
#endif
}

}  // namespace aios::userland
