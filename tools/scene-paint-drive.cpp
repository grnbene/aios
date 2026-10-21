// Renders the CANVAS PAINTER's own scenes — the CPU ones, not the shader's —
// to a file, so the picture a machine without a GPU puts on its glass can be
// looked at rather than described. The desktop routes every scene but the
// beach to the device when there is one, so these are otherwise unreachable
// on a machine with a GPU and there was no way to compare them.
//
//   aios-scene-paint-drive [WxH] [scene] [--text "..."] [--shot out.ppm]
#include "aios/userland/canvas.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace aios;
using namespace aios::userland;

int main(int argc, char** argv) {
  int W = 1920, H = 1080, scene = 4;
  uint64_t now_ms = 12000;
  std::string shot = "scene.ppm", text;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--shot" && i + 1 < argc) { shot = argv[++i]; continue; }
    if (a == "--text" && i + 1 < argc) { text = argv[++i]; continue; }
    if (a == "--at" && i + 1 < argc) { now_ms = (uint64_t)std::atoll(argv[++i]); continue; }
    unsigned w = 0, h = 0;
    if (std::sscanf(a.c_str(), "%ux%u", &w, &h) == 2) { W = (int)w; H = (int)h; continue; }
    scene = std::atoi(a.c_str());
  }
  CanvasState st;
  st.composed_text = text;
  std::vector<uint32_t> buf((size_t)W * (size_t)H, 0xFF000000u);
  paint_scene(buf, W, H, now_ms, st, scene);

  std::ofstream f(shot, std::ios::binary);
  f << "P6\n" << W << " " << H << "\n255\n";
  for (size_t i = 0; i < buf.size(); ++i) {
    const uint32_t p = buf[i];
    const char rgb[3] = {(char)((p >> 16) & 0xFF), (char)((p >> 8) & 0xFF), (char)(p & 0xFF)};
    f.write(rgb, 3);
  }
  std::printf("painted scene %d at %dx%d -> %s\n", scene, W, H, shot.c_str());
  return 0;
}
