// Decode an image with the kernel's own decoders and write it out as PPM, so
// what this machine thinks a picture looks like can be compared against what an
// independent decoder thinks. "The JPEG decoded" is then a measurement rather
// than a claim.
//
//   aios-image-check <in.(png|gif|jpg)> <out.ppm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "aios/userland/web.hpp"

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: aios-image-check <in> <out.ppm>\n"); return 2; }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string bytes = ss.str();

  auto img = aios::userland::web::decode_image(
      aios::ByteSpan((const uint8_t*)bytes.data(), bytes.size()));
  if (!img.ok()) { std::fprintf(stderr, "%s: %s\n", argv[1], img.error().message.c_str()); return 1; }

  std::ofstream out(argv[2], std::ios::binary);
  out << "P6\n" << img->w << " " << img->h << "\n255\n";
  for (uint32_t px : img->pixels) {
    char rgb[3] = {(char)((px >> 16) & 0xFF), (char)((px >> 8) & 0xFF), (char)(px & 0xFF)};
    out.write(rgb, 3);
  }
  std::printf("%s: %dx%d\n", argv[1], img->w, img->h);
  return 0;
}
