// Render the workspace desktop exactly as the bare-metal `desktop` command
// sets it up (metal/arm64/desktop_shell.cpp), at any size, to a PPM.
//
// The Pi takes ~30 s per boot and ~30 s per screenshot; this renders the
// same compositor code path on the host in well under a second, so layout at
// the display's native size (1920x1080 on the bench monitor) can be checked
// and fixed without the board.
//
//   aios-desktop-render <root> [WxH] [out.ppm] [frames]
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/userland/compositor.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace aios;

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: aios-desktop-render <root> [WxH] [out.ppm] [frames]\n"); return 2; }
  uint32_t W = 1920, H = 1080;
  if (argc > 2) std::sscanf(argv[2], "%ux%u", &W, &H);
  const std::string out = argc > 3 ? argv[3] : "desktop.ppm";
  const int frames = argc > 4 ? std::atoi(argv[4]) : 3;

  KernelConfig cfg;
  cfg.root = argv[1];
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::fprintf(stderr, "boot: %s\n", k.error().str().c_str()); return 1; }
  Kernel& kernel = **k;

  userland::HardwareCompositor wm(W, H);
  wm.set_async_commands(false);
  wm.set_canvas_mode(false);
  wm.init_default_workspace(kernel);
  for (int i = 0; i < frames; ++i) wm.render_desktop(kernel);

  const auto& fb = wm.framebuffer();
  FILE* f = std::fopen(out.c_str(), "wb");
  if (!f) { std::perror(out.c_str()); return 1; }
  std::fprintf(f, "P6\n%u %u\n255\n", W, H);
  for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i) {
    const uint32_t p = fb[i];
    const unsigned char rgb[3] = {static_cast<unsigned char>(p >> 16), static_cast<unsigned char>(p >> 8), static_cast<unsigned char>(p)};
    std::fwrite(rgb, 1, 3, f);
  }
  std::fclose(f);
  std::printf("wrote %s (%ux%u, %d frames)\n", out.c_str(), W, H, frames);
  return 0;
}
