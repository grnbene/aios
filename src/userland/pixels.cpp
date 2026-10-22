// Pixels into an ARGB buffer: the drawing every painter shares - the desktop
// (compositor.cpp), faces wherever they are shown, and the web pane's text.
// Moved here from the compositor unchanged, so a face can be drawn without a
// desktop and draws exactly what it drew on one.
#include "aios/userland/pixels.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace aios::userland::px {

GammaTables::GammaTables() {
  for (int i = 0; i < 256; ++i) {
    double v = i / 255.0;
    double lin = std::pow(v, 2.2);
    srgb_to_lin[i] = static_cast<uint16_t>(std::round(lin * 65535.0));

    if (i == 0) {
      font_cov_smooth[0] = 0;
    } else {
      // The coverage transfer curve.
      //
      // This was 0.80, called "stem darkening". Below 1.0 it INFLATES partial
      // coverage: a pixel the rasteriser reported a quarter covered is drawn
      // a third inked, a half-covered one nearly three fifths. Applied to
      // every edge pixel of every glyph at once, that is not darkening a
      // stem, it is spreading one — and a spread edge is what reads as out of
      // focus.
      //
      // Above 1.0 the opposite: partial coverage is held back, so the edge
      // falls away faster and the stem keeps its own width. Dumped the same
      // text-heavy frame at 0.80, 1.00, 1.25 and 1.45 and compared them: the
      // smear thins out all the way up, but past about 1.25 the strokes
      // themselves start going faint, which costs more readability than the
      // haze did. 1.25 is where it stops being blurry without becoming weak.
      static const double kCovGamma = [] {
        if (const char* e = std::getenv("AIOS_FONT_GAMMA")) {
          const double g = std::atof(e);
          if (g > 0.2 && g < 4.0) return g;
        }
        return 1.25;
      }();
      double smoothed = std::pow(v, kCovGamma);
      int sc = static_cast<int>(std::round(smoothed * 255.0));
      font_cov_smooth[i] = static_cast<uint8_t>(sc > 255 ? 255 : sc);
    }
  }
  // The 64K reverse table used to cost 65536 std::pow() calls before main(),
  // on every boot, with no FPU help on the metal. It only ever holds 256
  // distinct values, so build it from the 256 thresholds instead: output j
  // begins at the first linear index that rounds up to j, i.e.
  // ceil(65535 * ((j - 0.5)/255)^2.2). Bit-identical to the pow() loop,
  // 256 transcendentals instead of 65536.
  int at = 0;
  for (int j = 1; j <= 255; ++j) {
    double edge = (j - 0.5) / 255.0;
    double start_d = std::ceil(std::pow(edge, 2.2) * 65535.0);
    int start = start_d < 0.0 ? 0 : (start_d > 65536.0 ? 65536 : static_cast<int>(start_d));
    for (; at < start && at <= 65535; ++at) lin_to_srgb[at] = static_cast<uint8_t>(j - 1);
    if (start > at) at = start;
  }
  for (; at <= 65535; ++at) lin_to_srgb[at] = 255;
}

const GammaTables& gamma() {
  static const GammaTables tables;
  return tables;
}

namespace {
const GammaTables& g_gamma = gamma();

static inline void blend_px_lin(uint32_t& dst, uint32_t color, uint32_t lin_sr, uint32_t lin_sg,
                                uint32_t lin_sb, uint32_t alpha) {
  if (alpha >= 255) { dst = color; return; }

  uint32_t d = dst;
  uint32_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;

  uint32_t a = static_cast<uint32_t>(alpha);
  uint32_t inv_a = 255 - a;

  uint32_t lin_dr = g_gamma.srgb_to_lin[dr];
  uint32_t lin_dg = g_gamma.srgb_to_lin[dg];
  uint32_t lin_db = g_gamma.srgb_to_lin[db];

  uint32_t lin_r = (lin_sr * a + lin_dr * inv_a) / 255;
  uint32_t lin_g = (lin_sg * a + lin_dg * inv_a) / 255;
  uint32_t lin_b = (lin_sb * a + lin_db * inv_a) / 255;

  if (lin_r > 65535) lin_r = 65535;
  if (lin_g > 65535) lin_g = 65535;
  if (lin_b > 65535) lin_b = 65535;

  uint32_t r = g_gamma.lin_to_srgb[lin_r];
  uint32_t g = g_gamma.lin_to_srgb[lin_g];
  uint32_t b = g_gamma.lin_to_srgb[lin_b];

  dst = 0xFF000000u | (r << 16) | (g << 8) | b;
}

static inline void blend_px(uint32_t& dst, uint32_t color, uint32_t alpha) {
  blend_px_lin(dst, color, g_gamma.srgb_to_lin[(color >> 16) & 0xFF],
               g_gamma.srgb_to_lin[(color >> 8) & 0xFF], g_gamma.srgb_to_lin[color & 0xFF], alpha);
}


}  // namespace

void blend_px_linear(uint32_t& dst, uint32_t color, uint32_t alpha) { blend_px(dst, color, alpha); }

void fill_rect(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, int w, int h, uint32_t color) {
  // Clipped on BOTH sides. A rect that starts past the right or bottom edge —
  // which is what happens the moment a window is narrower than the content
  // drawn into it, and every window is, on a screen smaller than the one the
  // layout was written for — leaves x1 < x0, and std::fill over a reversed
  // range never reaches its end: it overwrites memory for as long as the
  // machine is powered on. On a hosted build that is a segfault; on the metal
  // it is a kernel that stops answering with no way to ask why.
  if (w <= 0 || h <= 0 || buf_w <= 0 || buf_h <= 0) return;
  int x0 = std::max(0, x);
  int y0 = std::max(0, y);
  int x1 = std::min(buf_w, x + w);
  int y1 = std::min(buf_h, y + h);
  if (x1 <= x0 || y1 <= y0) return;
  if (buf.size() < static_cast<size_t>(buf_w) * static_cast<size_t>(buf_h)) return;
  for (int py = y0; py < y1; ++py) {
    uint32_t* row = &buf[py * buf_w + x0];
    std::fill(row, row + (x1 - x0), color);
  }
}

void rect_outline(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, int w, int h, uint32_t color) {
  fill_rect(buf, buf_w, buf_h, x, y, w, 1, color);
  fill_rect(buf, buf_w, buf_h, x, y + h - 1, w, 1, color);
  fill_rect(buf, buf_w, buf_h, x, y, 1, h, color);
  fill_rect(buf, buf_w, buf_h, x + w - 1, y, 1, h, color);
}

void rounded_rect(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, int w, int h, int radius, uint32_t color) {
  if (w <= 0 || h <= 0) return;
  int r = radius;
  if (r < 0) r = 0;
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  // The interior is exact; only the corners need coverage. Three rects and
  // four small arcs, rather than the old stack of shrinking rectangles that
  // made every rounded corner a visible staircase.
  fill_rect(buf, buf_w, buf_h, x + r, y, w - 2 * r, h, color);
  fill_rect(buf, buf_w, buf_h, x, y + r, r, h - 2 * r, color);
  fill_rect(buf, buf_w, buf_h, x + w - r, y + r, r, h - 2 * r, color);
  if (!r) return;
  const double rr = r;
  const int cx[4] = {x, x + w - r, x, x + w - r};
  const int cy[4] = {y, y, y + h - r, y + h - r};
  for (int c = 0; c < 4; ++c) {
    double ox = (c == 0 || c == 2) ? x + rr : x + w - rr;
    double oy = (c == 0 || c == 1) ? y + rr : y + h - rr;
    for (int dy = 0; dy < r; ++dy) {
      for (int dx = 0; dx < r; ++dx) {
        int px = cx[c] + dx, py = cy[c] + dy;
        double ddx = px + 0.5 - ox, ddy = py + 0.5 - oy;
        double d = std::sqrt(ddx * ddx + ddy * ddy);
        double cov = rr + 0.5 - d;
        if (cov <= 0.0) continue;
        if (cov > 1.0) cov = 1.0;
        blend(buf, buf_w, buf_h, px, py, color, static_cast<int>(cov * 255.0 + 0.5));
      }
    }
  }
}

void blend(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y,
                                  uint32_t color, int alpha) {
  if (alpha <= 0 || x < 0 || y < 0 || x >= buf_w || y >= buf_h) return;
  size_t at = static_cast<size_t>(y) * static_cast<size_t>(buf_w) + static_cast<size_t>(x);
  if (at >= buf.size()) return;
  blend_px(buf[at], color, static_cast<uint32_t>(alpha));
}

void round_outline(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y,
                                               int w, int h, int radius, uint32_t edge) {
  if (w <= 0 || h <= 0) return;
  int r = radius;
  if (r < 0) r = 0;
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  fill_rect(buf, buf_w, buf_h, x + r, y, w - 2 * r, 1, edge);
  fill_rect(buf, buf_w, buf_h, x + r, y + h - 1, w - 2 * r, 1, edge);
  fill_rect(buf, buf_w, buf_h, x, y + r, 1, h - 2 * r, edge);
  fill_rect(buf, buf_w, buf_h, x + w - 1, y + r, 1, h - 2 * r, edge);
  if (!r) return;
  const double rr = r;
  const int cx[4] = {x, x + w - r, x, x + w - r};
  const int cy[4] = {y, y, y + h - r, y + h - r};
  for (int c = 0; c < 4; ++c) {
    double ox = (c == 0 || c == 2) ? x + rr : x + w - rr;
    double oy = (c == 0 || c == 1) ? y + rr : y + h - rr;
    for (int dy = 0; dy < r; ++dy) {
      for (int dx = 0; dx < r; ++dx) {
        int px = cx[c] + dx, py = cy[c] + dy;
        double ddx = px + 0.5 - ox, ddy = py + 0.5 - oy;
        double d = std::sqrt(ddx * ddx + ddy * ddy);
        // A one-pixel-wide arc, its edges feathered by how far the pixel's
        // centre is from the ideal curve.
        double cov = 1.0 - std::fabs(d - (rr - 0.5));
        if (cov <= 0.0) continue;
        if (cov > 1.0) cov = 1.0;
        blend(buf, buf_w, buf_h, px, py, edge, static_cast<int>(cov * 255.0 + 0.5));
      }
    }
  }
}

}  // namespace aios::userland::px
