// Every glyph of every face, drawn from the atlas as the engine reads it (Face::cov(), which inflates the
// deflated blob on first use), is the same pixels as the generator's own copy in metal/font_atlas.json.
// Run with the path to font_atlas.json.

#include "aios/core/json.hpp"
#include "aios/userland/font_atlas.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace aios::userland::fonts;

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: font-atlas-check <metal/font_atlas.json>\n"); return 2; }
  std::ifstream in(argv[1]);
  std::stringstream ss;
  ss << in.rdbuf();
  auto doc = aios::json::parse(ss.str());
  if (!doc.ok()) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }

  // Many threads asking for the same face at once get the same blob (one inflate kept, the rest dropped).
  {
    const Face& f = *kFaces[0].face;
    std::vector<const uint8_t*> got(8, nullptr);
    std::vector<std::thread> ts;
    for (size_t i = 0; i < got.size(); ++i) ts.emplace_back([&, i] { got[i] = f.cov(); });
    for (auto& t : ts) t.join();
    for (auto* p : got)
      if (p != got[0]) { std::printf("FAIL threads saw different blobs for %s\n", kFaces[0].name); return 1; }
  }

  size_t faces = 0, glyphs = 0, bad = 0, packed = 0, raw = 0;
  for (uint32_t i = 0; i < kFaceCount; ++i) {
    const char* name = kFaces[i].name;
    const Face& f = *kFaces[i].face;
    const auto& jf = (*doc)[name];
    if (!jf.is_object()) { std::printf("FAIL %s is not in the json\n", name); ++bad; continue; }
    const uint8_t* cov = f.cov();
    packed += f.covz_n;
    raw += f.cov_n;
    for (const auto& [code, jg] : jf["glyphs"].members()) {
      const Glyph* g = f.find((uint32_t)std::stoul(code));
      if (!g) { std::printf("FAIL %s has no glyph %s\n", name, code.c_str()); ++bad; continue; }
      const auto& pix = jg["pix"].items();
      if (pix.size() != (size_t)g->w * g->h || g->off + pix.size() > f.cov_n) {
        std::printf("FAIL %s glyph %s: %zu pixels for a %dx%d box\n", name, code.c_str(), pix.size(), g->w, g->h);
        ++bad;
        continue;
      }
      for (size_t p = 0; p < pix.size(); ++p) {
        if (cov[g->off + p] != (uint8_t)pix[p].num()) {
          std::printf("FAIL %s glyph %s pixel %zu: %d, the generator drew %d\n", name, code.c_str(), p, cov[g->off + p],
                      (int)pix[p].num());
          ++bad;
          break;
        }
      }
      ++glyphs;
    }
    ++faces;
  }
  std::printf("%s %zu faces, %zu glyphs; coverage %.2f MB kept as %.2f MB\n", bad ? "FAIL" : "ok  ", faces, glyphs,
              raw / 1048576.0, packed / 1048576.0);
  return bad ? 1 : 0;
}
