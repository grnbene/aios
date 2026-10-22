#pragma once
// Pixels into an ARGB buffer (0xAARRGGBB, row-major, `w` by `h`): the shapes
// and blend every painter shares. The desktop's own drawing calls these, and
// so do faces wherever they are shown, so the same frame comes out of both.
// Every call clips to the buffer; nothing is drawn outside it.
#include <cstdint>
#include <vector>

namespace aios::userland::px {

// sRGB <-> linear light, and the coverage curve glyph edges are drawn through.
struct GammaTables {
  uint16_t srgb_to_lin[256];
  uint8_t lin_to_srgb[65536];
  uint8_t font_cov_smooth[256];
  GammaTables();
};
const GammaTables& gamma();

// Source-over in linear light, alpha 0..255, into one pixel.
void blend_px_linear(uint32_t& dst, uint32_t color, uint32_t alpha);
// The same at (x, y) of a buffer; a pixel outside it is not touched.
void blend(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, uint32_t color, int alpha);

void fill_rect(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, int w, int h, uint32_t color);
void rect_outline(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, int w, int h, uint32_t color);
// Filled with antialiased corners of `radius` (clamped to half the smaller side).
void rounded_rect(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, int w, int h, int radius, uint32_t color);
// A one-pixel outline with antialiased corners.
void round_outline(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, int w, int h, int radius, uint32_t color);

}  // namespace aios::userland::px
