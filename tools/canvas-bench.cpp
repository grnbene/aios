// What the canvas costs to paint, per scene, on this machine.
//
// The compositor is a CPU software rasteriser: every pixel of every frame,
// including the three procedural scenes, is computed by the processor. Before
// moving any of that onto the GPU it is worth knowing what it actually costs,
// because a transfer back across PCIe is not free either and "we moved it to
// the GPU" is not the same claim as "it got faster".
//
//   aios-canvas-bench <root> [WxH] [frames]
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/userland/canvas.hpp"
#include "aios/userland/compositor.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace aios;

namespace {
double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: aios-canvas-bench <root> [WxH] [frames]\n"); return 2; }
  uint32_t W = 1920, H = 1080;
  if (argc > 2) std::sscanf(argv[2], "%ux%u", &W, &H);
  int frames = argc > 3 ? std::atoi(argv[3]) : 60;
  if (frames < 1) frames = 60;

  KernelConfig cfg;
  cfg.root = argv[1];
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::fprintf(stderr, "boot: %s\n", k.error().str().c_str()); return 1; }
  Kernel& kernel = **k;

  AuthorityProfile profile;
  auto me = kernel.resident("canvas-bench", profile);
  if (!me.ok()) { std::fprintf(stderr, "resident: %s\n", me.error().str().c_str()); return 1; }
  Surface console = kernel.as(*me);

  userland::HardwareCompositor surface(W, H);
  std::vector<uint32_t> buf((size_t)W * H, 0xFF0A0A0A);

  std::printf("canvas at %ux%u, %d frames per scene\n\n", W, H, frames);
  std::printf("  %-22s %10s %10s %12s\n", "SCENE", "ms/frame", "fps", "Mpixel/s");

  static const char* names[3] = {"beach & moon", "cyber matrix", "quantum orb"};
  for (int scene = 0; scene < 3; ++scene) {
    userland::CanvasState st;
    st.set_every_scene(scene);
    st.scene_file_value = scene;
    st.hide_panels = false;
    st.sweep(kernel, 0);

    // One frame first: the scenes tabulate their lookup tables on the way
    // through, and timing that once would be timing the wrong thing.
    userland::CanvasFeed feed;
    userland::paint_canvas(surface, buf, W, H, kernel, console, feed, 0, st);

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
      userland::paint_canvas(surface, buf, W, H, kernel, console, feed, (uint64_t)i * 16, st);
    }
    double total = ms_since(t0);
    double per = total / frames;
    std::printf("  %-22s %10.2f %10.1f %12.1f\n", names[scene], per, per > 0 ? 1000.0 / per : 0.0,
                per > 0 ? ((double)W * H) / (per * 1000.0) : 0.0);
  }

  std::printf("\n  a frame is %.1f MiB; reading one back over PCIe at 8 GB/s costs about %.2f ms\n",
              ((double)W * H * 4) / (1024 * 1024), ((double)W * H * 4) / 8e9 * 1000.0);
  return 0;
}
