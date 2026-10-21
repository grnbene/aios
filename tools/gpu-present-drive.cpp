// Drives the GPU display path on its own, before it is wired into the
// compositor: open a window, hand its surface to the presenter, render frames
// on the GPU, and say what they cost.
//
// The point of measuring here rather than after integration is that if the
// GPU path is not faster than the CPU rasteriser it replaces, it should not
// replace it — and the only way to know is to run both and look.
//
//   aios-gpu-present-drive [WxH] [frames] [scene] [--shot out.ppm]
//                          [--composed desc.txt]   render a composed scene
//                          [--scale N]             march the background at 1/N,
//                                                  the way the desktop does
//                          [--tod F]               the hour on the shoreline:
//                                                  0 midnight, .25 dawn, .5
//                                                  midday, .75 dusk (scene 0)
#include "aios/userland/gpu_present.hpp"
#include "aios/userland/scene_description.hpp"
#include <fstream>
#include <sstream>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(AIOS_HAS_SDL2)
#include <SDL.h>
#include <SDL_vulkan.h>
#endif

using namespace aios;

int main(int argc, char** argv) {
#if !defined(AIOS_HAS_SDL2)
  std::fprintf(stderr, "this build has no SDL2, so there is no window to present into\n");
  return 2;
#else
  uint32_t W = 1920, H = 1080;
  int frames = 240, scene = 1;
  std::string shot;
  std::string composed_path;
  int scale = 1;
  float tod = 0.0f;
  double freeze = 0.0; bool froze = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--shot" && i + 1 < argc) { shot = argv[++i]; continue; }
    if (a == "--scale" && i + 1 < argc) { scale = std::atoi(argv[++i]); continue; }
    if (a == "--tod" && i + 1 < argc) { tod = (float)std::atof(argv[++i]); continue; }
    // Hold the scene's clock still. Two runs of an animated scene are two
    // different instants, so a pixel comparison between builds means nothing
    // until both are asked for the SAME second.
    if (a == "--freeze" && i + 1 < argc) { freeze = std::atof(argv[++i]); froze = true; continue; }
    if (a == "--composed" && i + 1 < argc) { composed_path = argv[++i]; continue; }
    unsigned w = 0, h = 0;
    if (std::sscanf(a.c_str(), "%ux%u", &w, &h) == 2) { W = w; H = h; continue; }
    if (frames == 240) { frames = std::atoi(a.c_str()); continue; }
    scene = std::atoi(a.c_str());
  }
  if (frames < 1) frames = 240;

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window* win = SDL_CreateWindow("AIOS - GPU display", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     (int)W, (int)H, SDL_WINDOW_SHOWN | SDL_WINDOW_VULKAN);
  if (!win) {
    std::fprintf(stderr, "no Vulkan window: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  unsigned ext_count = 0;
  SDL_Vulkan_GetInstanceExtensions(win, &ext_count, nullptr);
  std::vector<const char*> exts(ext_count);
  SDL_Vulkan_GetInstanceExtensions(win, &ext_count, exts.data());
  std::printf("window asks for %u instance extension(s):", ext_count);
  for (auto e : exts) std::printf(" %s", e);
  std::printf("\n");

  std::string why;
  auto inst = userland::GpuPresenter::make_instance(exts.data(), ext_count, why);
  if (!inst.ok()) { std::fprintf(stderr, "instance: %s\n", why.c_str()); return 1; }

  // SDL declares the Vulkan handle types itself; VK_NULL_HANDLE belongs to
  // the Vulkan headers, which this build does not require.
  VkSurfaceKHR surface{};
  if (!SDL_Vulkan_CreateSurface(win, (VkInstance)*inst, &surface)) {
    std::fprintf(stderr, "SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    return 1;
  }

  // Scoped, so every Vulkan object it holds is destroyed while the surface it
  // was made against still exists. Destroying the window first took the surface
  // out from under a live swapchain and the process dumped core — which is how
  // the first failing run of this drive ended, hiding the real error above it.
  int presented = 0;
  {
  userland::GpuPresenter gpu;
  auto attached = gpu.attach(*inst, reinterpret_cast<void*>(surface), W, H);
  if (!attached.ok()) {
    std::fprintf(stderr, "attach: %s\n", gpu.unavailable().c_str());
    return 1;
  }
  std::printf("presenting on %s\n", gpu.device_name().c_str());
  std::printf("background marched at 1/%d in each axis\n", scale);
  // A scene the machine composed, read from the text it was written as. This
  // is the whole point of the drive: the picture is not in the binary.
  if (!composed_path.empty()) {
    std::ifstream f(composed_path);
    if (!f) { std::fprintf(stderr, "cannot read %s\n", composed_path.c_str()); return 1; }
    std::stringstream ss;
    ss << f.rdbuf();
    auto desc = userland::SceneDescription::from_text(ss.str());
    if (!desc.ok()) {
      std::fprintf(stderr, "that description does not parse: %s\n", desc.error().message.c_str());
      return 1;
    }
    auto sent = gpu.set_scene_description(desc->to_floats());
    if (!sent.ok()) {
      std::fprintf(stderr, "upload: %s\n", sent.error().message.c_str());
      return 1;
    }
    std::printf("composed: %s (%zu solid(s))\n", desc->summary.c_str(), desc->solids.size());
    scene = 4;
  }
  // Which path it is cannot be said yet: the swapchain is built on the first
  // present, so asking now always answers "copy" whatever the surface allows.
  bool said_path = false;

  // An overlay that changes once, so the upload path is exercised and the
  // "only when it changed" rule is visible in the counter at the end.
  std::vector<uint32_t> overlay((size_t)W * H, 0);
  for (uint32_t y = 40; y < 40 + 90 && y < H; ++y) {
    for (uint32_t x = 40; x < 40 + 520 && x < W; ++x) {
      bool edge = y < 42 || y >= 128 || x < 42 || x >= 558;
      overlay[(size_t)y * W + x] = edge ? 0xFF50E3C2u : 0xC00F172Au;
    }
  }

  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < frames; ++i) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT) { i = frames; break; }
      if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        gpu.resize((uint32_t)ev.window.data1, (uint32_t)ev.window.data2);
      }
    }
    double seconds = froze ? freeze
                           : std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    gpu.set_time_of_day(tod);
    auto r = gpu.present(scene, seconds, overlay.data(), 1, scale);
    if (!r.ok()) { std::fprintf(stderr, "present: %s\n", r.error().str().c_str()); break; }
    if (!said_path) {
      std::printf("  %s\n", gpu.direct_to_swapchain()
                  ? "the shader writes the swapchain image itself — nothing is copied"
                  : "into our own render target, copied into the swapchain");
      said_path = true;
    }
    presented++;
  }
  double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

  std::printf("\n  %d frames in %.0f ms = %.2f ms/frame (%.0f fps)\n", presented, total,
              presented ? total / presented : 0.0, total > 0 ? presented * 1000.0 / total : 0.0);
  std::printf("  overlay uploaded %llu time(s) for %d frames\n",
              (unsigned long long)gpu.cost().overlay_uploads, presented);
  std::printf("  note: presentation is FIFO, so the display's refresh rate is the ceiling here;\n"
              "        what this shows is that the frames are produced and shown by the GPU.\n");

  // --shot was advertised in the usage line and did nothing but sleep. Write
  // the frame the device produced, which is the only way to see this path on a
  // machine whose window system will not hand a Vulkan window to a screen grab.
  if (!shot.empty()) {
    std::vector<uint32_t> px;
    auto got = gpu.capture(px);
    if (!got.ok()) {
      std::fprintf(stderr, "capture: %s\n", got.error().str().c_str());
    } else {
      FILE* f = std::fopen(shot.c_str(), "wb");
      if (!f) { std::fprintf(stderr, "cannot write %s\n", shot.c_str()); }
      else {
        std::fprintf(f, "P6\n%u %u\n255\n", W, H);
        std::vector<uint8_t> row((size_t)W * 3);
        for (uint32_t y = 0; y < H; ++y) {
          for (uint32_t x = 0; x < W; ++x) {
            const uint32_t w = px[(size_t)y * W + x];
            row[(size_t)x * 3 + 0] = (uint8_t)(w >> 16);
            row[(size_t)x * 3 + 1] = (uint8_t)(w >> 8);
            row[(size_t)x * 3 + 2] = (uint8_t)w;
          }
          std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
        std::printf("  wrote %s — %ux%u, straight off the device\n", shot.c_str(), W, H);
      }
    }
  }
  }
  SDL_DestroyWindow(win);
  SDL_Quit();
  return presented > 0 ? 0 : 1;
#endif
}
