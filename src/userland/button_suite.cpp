#include "aios/userland/button_suite.hpp"
#include "aios/userland/font_atlas.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aios::userland {

namespace {

inline uint32_t clamp_u8(float v) {
  int i = static_cast<int>(v + 0.5f);
  return static_cast<uint32_t>(i < 0 ? 0 : (i > 255 ? 255 : i));
}

inline void unpack_rgb(uint32_t c, float& r, float& g, float& b) {
  r = static_cast<float>((c >> 16) & 0xFF);
  g = static_cast<float>((c >> 8) & 0xFF);
  b = static_cast<float>(c & 0xFF);
}

inline uint32_t pack_rgb(float r, float g, float b, uint32_t a = 0xFF) {
  return (a << 24) | (clamp_u8(r) << 16) | (clamp_u8(g) << 8) | clamp_u8(b);
}

// Distance from point (px, py) to line segment (x0, y0)-(x1, y1)
float dist_to_segment(float px, float py, float x0, float y0, float x1, float y1) {
  float dx = x1 - x0;
  float dy = y1 - y0;
  float len_sq = dx * dx + dy * dy;
  if (len_sq < 1e-6f) {
    float ex = px - x0, ey = py - y0;
    return std::sqrt(ex * ex + ey * ey);
  }
  float t = ((px - x0) * dx + (py - y0) * dy) / len_sq;
  t = std::max(0.0f, std::min(1.0f, t));
  float qx = x0 + t * dx;
  float qy = y0 + t * dy;
  float rx = px - qx;
  float ry = py - qy;
  return std::sqrt(rx * rx + ry * ry);
}

// Draw text from internal font atlas
void draw_text_simple(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                      int x, int y, const std::string& text, uint32_t color,
                      const fonts::Face& face) {
  int pen = x;
  int line_top = y;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    const fonts::Glyph& g = face.at(c);
    if (g.w && g.h && buf.size() >= static_cast<size_t>(buf_w) * static_cast<size_t>(buf_h)) {
      int glyph_x = pen + g.bx;
      for (int gy = 0; gy < static_cast<int>(g.h); ++gy) {
        int py = line_top + g.by + gy;
        if (py < 0 || py >= buf_h) continue;
        const uint8_t* cov_row = face.cov + g.off + static_cast<size_t>(gy) * g.w;
        for (int gx = 0; gx < static_cast<int>(g.w); ++gx) {
          int px = glyph_x + gx;
          if (px < 0 || px >= buf_w) continue;
          uint8_t cov = cov_row[gx];
          if (cov) {
            ButtonSuite::blend_pixel(buf, buf_w, buf_h, px, py, color, cov);
          }
        }
      }
    }
    pen += g.adv;
    if (pen >= buf_w) break;
  }
}

int text_width_simple(const std::string& text, const fonts::Face& face) {
  int w = 0;
  for (char c : text) {
    w += face.at(c).adv;
  }
  return w;
}

}  // namespace

void ButtonSuite::blend_pixel(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                              int x, int y, uint32_t color, int alpha) {
  if (x < 0 || x >= buf_w || y < 0 || y >= buf_h) return;
  if (alpha <= 0) return;
  if (alpha > 255) alpha = 255;
  size_t idx = static_cast<size_t>(y) * static_cast<size_t>(buf_w) + static_cast<size_t>(x);
  if (idx >= buf.size()) return;

  uint32_t dst = buf[idx];
  int inv_a = 255 - alpha;
  uint32_t sr = (color >> 16) & 0xFF, sg = (color >> 8) & 0xFF, sb = color & 0xFF;
  uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;

  uint32_t r = (sr * alpha + dr * inv_a) / 255;
  uint32_t g = (sg * alpha + dg * inv_a) / 255;
  uint32_t b = (sb * alpha + db * inv_a) / 255;
  buf[idx] = (0xFF << 24) | (r << 16) | (g << 8) | b;
}

void ButtonSuite::draw_line_aa(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                               float x0, float y0, float x1, float y1,
                               uint32_t color, float thickness) {
  float min_x = std::max(0.0f, std::min(x0, x1) - thickness - 1.5f);
  float max_x = std::min(static_cast<float>(buf_w - 1), std::max(x0, x1) + thickness + 1.5f);
  float min_y = std::max(0.0f, std::min(y0, y1) - thickness - 1.5f);
  float max_y = std::min(static_cast<float>(buf_h - 1), std::max(y0, y1) + thickness + 1.5f);

  float half_t = thickness * 0.5f;

  for (int py = static_cast<int>(min_y); py <= static_cast<int>(max_y); ++py) {
    for (int px = static_cast<int>(min_x); px <= static_cast<int>(max_x); ++px) {
      float d = dist_to_segment(px + 0.5f, py + 0.5f, x0, y0, x1, y1);
      float cov = 0.5f + half_t - d;
      cov = std::max(0.0f, std::min(1.0f, cov));
      if (cov > 0.0f) {
        int a = static_cast<int>(cov * 255.0f);
        blend_pixel(buf, buf_w, buf_h, px, py, color, a);
      }
    }
  }
}

void ButtonSuite::draw_circle(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                              float cx, float cy, float radius,
                              uint32_t fill_color, uint32_t outline_color, float outline_width) {
  float bound = radius + outline_width + 1.5f;
  int min_x = std::max(0, static_cast<int>(cx - bound));
  int max_x = std::min(buf_w - 1, static_cast<int>(cx + bound));
  int min_y = std::max(0, static_cast<int>(cy - bound));
  int max_y = std::min(buf_h - 1, static_cast<int>(cy + bound));

  for (int py = min_y; py <= max_y; ++py) {
    for (int px = min_x; px <= max_x; ++px) {
      float dx = (px + 0.5f) - cx;
      float dy = (py + 0.5f) - cy;
      float d = std::sqrt(dx * dx + dy * dy);

      // Fill coverage
      if (fill_color != 0) {
        float fill_cov = 0.5f + radius - d;
        fill_cov = std::max(0.0f, std::min(1.0f, fill_cov));
        if (fill_cov > 0.0f) {
          blend_pixel(buf, buf_w, buf_h, px, py, fill_color, static_cast<int>(fill_cov * 255.0f));
        }
      }

      // Outline coverage
      if (outline_color != 0 && outline_width > 0.0f) {
        float edge_cov = 0.5f + outline_width * 0.5f - std::fabs(d - radius);
        edge_cov = std::max(0.0f, std::min(1.0f, edge_cov));
        if (edge_cov > 0.0f) {
          blend_pixel(buf, buf_w, buf_h, px, py, outline_color, static_cast<int>(edge_cov * 255.0f));
        }
      }
    }
  }
}

void ButtonSuite::draw_rounded_gradient_v(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                                          int x, int y, int w, int h, int radius,
                                          uint32_t c_top, uint32_t c_bottom) {
  if (w <= 0 || h <= 0) return;
  int r = std::min(radius, std::min(w / 2, h / 2));
  if (r < 0) r = 0;

  float tr, tg, tb, br, bg, bb;
  unpack_rgb(c_top, tr, tg, tb);
  unpack_rgb(c_bottom, br, bg, bb);
  // The same row interpolation draw_gradient_v uses, so a rounded gradient and
  // a square one of the same size shade identically.
  auto row_color = [&](int py) {
    float t = (h > 1) ? static_cast<float>(py - y) / static_cast<float>(h - 1) : 0.0f;
    t = std::max(0.0f, std::min(1.0f, t));
    return pack_rgb(tr + t * (br - tr), tg + t * (bg - tg), tb + t * (bb - tb));
  };

  // Solid middle rectangles
  int x0 = std::max(0, x);
  int x1 = std::min(buf_w, x + w);
  int y0 = std::max(0, y + r);
  int y1 = std::min(buf_h, y + h - r);

  for (int py = y0; py < y1; ++py) {
    const uint32_t fill = row_color(py);
    for (int px = x0; px < x1; ++px) {
      blend_pixel(buf, buf_w, buf_h, px, py, fill, 255);
    }
  }

  // Top and bottom horizontal spans between corners
  int tx0 = std::max(0, x + r);
  int tx1 = std::min(buf_w, x + w - r);
  for (int py = std::max(0, y); py < std::min(buf_h, y + r); ++py) {
    const uint32_t fill = row_color(py);
    for (int px = tx0; px < tx1; ++px) blend_pixel(buf, buf_w, buf_h, px, py, fill, 255);
  }
  for (int py = std::max(0, y + h - r); py < std::min(buf_h, y + h); ++py) {
    const uint32_t fill = row_color(py);
    for (int px = tx0; px < tx1; ++px) blend_pixel(buf, buf_w, buf_h, px, py, fill, 255);
  }

  if (r <= 0) return;

  // Antialiased 4 corner arcs
  const float cx[4] = {x + (float)r, x + w - (float)r, x + (float)r, x + w - (float)r};
  const float cy[4] = {y + (float)r, y + (float)r, y + h - (float)r, y + h - (float)r};

  for (int c = 0; c < 4; ++c) {
    int bx0 = (c == 0 || c == 2) ? x : x + w - r;
    int bx1 = (c == 0 || c == 2) ? x + r : x + w;
    int by0 = (c == 0 || c == 1) ? y : y + h - r;
    int by1 = (c == 0 || c == 1) ? y + r : y + h;

    for (int py = std::max(0, by0); py < std::min(buf_h, by1); ++py) {
      const uint32_t fill = row_color(py);
      for (int px = std::max(0, bx0); px < std::min(buf_w, bx1); ++px) {
        float dx = (px + 0.5f) - cx[c];
        float dy = (py + 0.5f) - cy[c];
        float d = std::sqrt(dx * dx + dy * dy);
        float cov = 0.5f + r - d;
        cov = std::max(0.0f, std::min(1.0f, cov));
        if (cov > 0.0f) {
          blend_pixel(buf, buf_w, buf_h, px, py, fill, static_cast<int>(cov * 255.0f));
        }
      }
    }
  }
}

// One colour is a gradient that does not change: the corner coverage above is
// the only copy of that arithmetic.
void ButtonSuite::draw_rounded_rect(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                                    int x, int y, int w, int h, int radius, uint32_t fill) {
  draw_rounded_gradient_v(buf, buf_w, buf_h, x, y, w, h, radius, fill, fill);
}

void ButtonSuite::draw_rounded_top_rim(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                                       int x, int y, int w, int h, int radius,
                                       uint32_t color, int alpha) {
  if (w <= 0 || h <= 0 || alpha <= 0) return;
  int r = std::min(radius, std::min(w / 2, h / 2));
  if (r < 0) r = 0;

  const int x0 = std::max(0, x), x1 = std::min(buf_w, x + w);
  for (int px = x0; px < x1; ++px) {
    const float cxf = px + 0.5f;
    float top_y = static_cast<float>(y);
    float up = 1.0f;                 // how much of this column's edge faces up
    if (r > 0 && (cxf < x + r || cxf > x + w - r)) {
      const float cx = (cxf < x + r) ? (x + (float)r) : (x + w - (float)r);
      const float dx = cxf - cx;
      const float inside = (float)r * (float)r - dx * dx;
      if (inside <= 0.0f) continue;  // past the corner: no surface here to catch light
      const float ny = std::sqrt(inside);
      top_y = (y + (float)r) - ny;
      // At the top of the arc the edge faces straight up and takes the full
      // highlight; by the corner's widest point it faces sideways and takes
      // none. That taper is what makes this read as light on an edge rather
      // than a bar laid across the top.
      up = ny / (float)r;
    }
    // The row just inside the outline, which is stroked along the edge itself.
    const int ry = static_cast<int>(std::floor(top_y)) + 1;
    blend_pixel(buf, buf_w, buf_h, px, ry, color, static_cast<int>(alpha * up));
  }
}

void ButtonSuite::draw_round_outline(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                                     int x, int y, int w, int h, int radius, uint32_t edge, float thickness) {
  if (w <= 0 || h <= 0) return;
  int r = std::min(radius, std::min(w / 2, h / 2));
  if (r < 0) r = 0;

  // Edges
  draw_line_aa(buf, buf_w, buf_h, x + r, y + 0.5f, x + w - r, y + 0.5f, edge, thickness);
  draw_line_aa(buf, buf_w, buf_h, x + r, y + h - 0.5f, x + w - r, y + h - 0.5f, edge, thickness);
  draw_line_aa(buf, buf_w, buf_h, x + 0.5f, y + r, x + 0.5f, y + h - r, edge, thickness);
  draw_line_aa(buf, buf_w, buf_h, x + w - 0.5f, y + r, x + w - 0.5f, y + h - r, edge, thickness);

  if (r <= 0) return;

  // Corner arcs
  const float cx[4] = {x + (float)r, x + w - (float)r, x + (float)r, x + w - (float)r};
  const float cy[4] = {y + (float)r, y + (float)r, y + h - (float)r, y + h - (float)r};

  for (int c = 0; c < 4; ++c) {
    int bx0 = (c == 0 || c == 2) ? x : x + w - r;
    int bx1 = (c == 0 || c == 2) ? x + r : x + w;
    int by0 = (c == 0 || c == 1) ? y : y + h - r;
    int by1 = (c == 0 || c == 1) ? y + r : y + h;

    for (int py = std::max(0, by0); py < std::min(buf_h, by1); ++py) {
      for (int px = std::max(0, bx0); px < std::min(buf_w, bx1); ++px) {
        float dx = (px + 0.5f) - cx[c];
        float dy = (py + 0.5f) - cy[c];
        float d = std::sqrt(dx * dx + dy * dy);
        float cov = 0.5f + thickness * 0.5f - std::fabs(d - (r - 0.5f));
        cov = std::max(0.0f, std::min(1.0f, cov));
        if (cov > 0.0f) {
          blend_pixel(buf, buf_w, buf_h, px, py, edge, static_cast<int>(cov * 255.0f));
        }
      }
    }
  }
}

void ButtonSuite::draw_gradient_v(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                                  int x, int y, int w, int h, uint32_t c_top, uint32_t c_bottom) {
  if (w <= 0 || h <= 0) return;
  float r0, g0, b0, r1, g1, b1;
  unpack_rgb(c_top, r0, g0, b0);
  unpack_rgb(c_bottom, r1, g1, b1);

  int x0 = std::max(0, x), x1 = std::min(buf_w, x + w);
  int y0 = std::max(0, y), y1 = std::min(buf_h, y + h);
  for (int py = y0; py < y1; ++py) {
    float t = (h > 1) ? (float)(py - y) / (float)(h - 1) : 0.0f;
    uint32_t fill = pack_rgb(r0 + (r1 - r0) * t, g0 + (g1 - g0) * t, b0 + (b1 - b0) * t);
    for (int px = x0; px < x1; ++px) blend_pixel(buf, buf_w, buf_h, px, py, fill, 255);
  }
}

void ButtonSuite::draw_polygon_aa(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                                  const std::vector<std::pair<float, float>>& pts,
                                  uint32_t fill_color, uint32_t outline_color, float outline_width) {
  if (pts.size() < 3) return;
  float min_x = static_cast<float>(buf_w), max_x = 0.0f;
  float min_y = static_cast<float>(buf_h), max_y = 0.0f;
  for (const auto& p : pts) {
    min_x = std::min(min_x, p.first);
    max_x = std::max(max_x, p.first);
    min_y = std::min(min_y, p.second);
    max_y = std::max(max_y, p.second);
  }

  int x0 = std::max(0, static_cast<int>(std::floor(min_x - 1.0f)));
  int x1 = std::min(buf_w - 1, static_cast<int>(std::ceil(max_x + 1.0f)));
  int y0 = std::max(0, static_cast<int>(std::floor(min_y - 1.0f)));
  int y1 = std::min(buf_h - 1, static_cast<int>(std::ceil(max_y + 1.0f)));

  const size_t n = pts.size();
  for (int py = y0; py <= y1; ++py) {
    const float y_sample = py + 0.5f;
    for (int px = x0; px <= x1; ++px) {
      const float x_sample = px + 0.5f;
      // Ray casting winding check
      bool inside = false;
      float min_dist = 1e9f;
      for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const float xi = pts[i].first, yi = pts[i].second;
        const float xj = pts[j].first, yj = pts[j].second;
        if (((yi > y_sample) != (yj > y_sample)) &&
            (x_sample < (xj - xi) * (y_sample - yi) / (yj - yi) + xi)) {
          inside = !inside;
        }
        float d = dist_to_segment(x_sample, y_sample, xi, yi, xj, yj);
        min_dist = std::min(min_dist, d);
      }
      float signed_dist = inside ? -min_dist : min_dist;
      float cov = 0.5f - signed_dist;
      cov = std::max(0.0f, std::min(1.0f, cov));
      if (cov > 0.0f) {
        blend_pixel(buf, buf_w, buf_h, px, py, fill_color, static_cast<int>(cov * 255.0f));
      }
    }
  }

  if (outline_color != 0 && outline_width > 0.0f) {
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
      draw_line_aa(buf, buf_w, buf_h, pts[j].first, pts[j].second, pts[i].first, pts[i].second, outline_color, outline_width);
    }
  }
}

void ButtonSuite::draw_bezier_aa(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                                 float x0, float y0, float cx, float cy, float x1, float y1,
                                 uint32_t color, float thickness) {
  const int steps = 16;
  float prev_x = x0, prev_y = y0;
  for (int i = 1; i <= steps; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(steps);
    float inv_t = 1.0f - t;
    float curr_x = inv_t * inv_t * x0 + 2.0f * inv_t * t * cx + t * t * x1;
    float curr_y = inv_t * inv_t * y0 + 2.0f * inv_t * t * cy + t * t * y1;
    draw_line_aa(buf, buf_w, buf_h, prev_x, prev_y, curr_x, curr_y, color, thickness);
    prev_x = curr_x;
    prev_y = curr_y;
  }
}

void ButtonSuite::draw_star(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                            float cx, float cy, float r_outer, float r_inner, int points,
                            uint32_t fill_color, uint32_t outline_color, float outline_width) {
  if (points < 3) return;
  std::vector<std::pair<float, float>> pts;
  pts.reserve(points * 2);
  const float step = 3.14159265f / static_cast<float>(points);
  for (int i = 0; i < points * 2; ++i) {
    float angle = static_cast<float>(i) * step - 1.5707963f;
    float r = (i % 2 == 0) ? r_outer : r_inner;
    pts.emplace_back(cx + std::cos(angle) * r, cy + std::sin(angle) * r);
  }
  draw_polygon_aa(buf, buf_w, buf_h, pts, fill_color, outline_color, outline_width);
}

void ButtonSuite::draw_drop_shadow(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                                   int x, int y, int w, int h, int radius, int blur, int shadow_alpha) {
  if (w <= 0 || h <= 0 || blur <= 0 || shadow_alpha <= 0) return;
  int x0 = std::max(0, x - blur);
  int x1 = std::min(buf_w - 1, x + w + blur);
  int y0 = std::max(0, y - blur / 2);
  int y1 = std::min(buf_h - 1, y + h + blur + blur / 2);

  const float r_f = static_cast<float>(radius);
  const float blur_f = static_cast<float>(blur);

  for (int py = y0; py <= y1; ++py) {
    float cy_point = py + 0.5f;
    for (int px = x0; px <= x1; ++px) {
      float cx_point = px + 0.5f;
      // Distance to rounded rect (x, y + blur/3, w, h)
      float rect_x = std::max(static_cast<float>(x + radius), std::min(static_cast<float>(x + w - radius), cx_point));
      float rect_y = std::max(static_cast<float>(y + blur / 3 + radius), std::min(static_cast<float>(y + blur / 3 + h - radius), cy_point));
      float dx = cx_point - rect_x;
      float dy = cy_point - rect_y;
      float dist = std::sqrt(dx * dx + dy * dy) - r_f;
      if (dist < blur_f) {
        float falloff = 1.0f - std::max(0.0f, dist) / blur_f;
        falloff = falloff * falloff;
        int a = static_cast<int>(falloff * shadow_alpha);
        blend_pixel(buf, buf_w, buf_h, px, py, 0x000000, a);
      }
    }
  }
}

Icon ButtonSuite::icon_from_name(const std::string& name) {
  if (name == "terminal" || name == "shell") return Icon::Terminal;
  if (name == "code" || name == "editor") return Icon::Code;
  if (name == "database" || name == "tables" || name == "fast_tables") return Icon::Database;
  if (name == "browser" || name == "web" || name == "globe") return Icon::Browser;
  if (name == "mail" || name == "message" || name == "email") return Icon::Mail;
  if (name == "inspector" || name == "settings" || name == "sliders") return Icon::Inspector;
  if (name == "flame" || name == "hot") return Icon::Flame;
  if (name == "box" || name == "sku" || name == "inventory") return Icon::Box;
  if (name == "sql") return Icon::Sql;
  if (name == "cluster" || name == "sys" || name == "nodes") return Icon::Cluster;
  if (name == "search") return Icon::Search;
  if (name == "play") return Icon::Play;
  if (name == "stop") return Icon::Stop;
  if (name == "close") return Icon::Close;
  if (name == "minimize") return Icon::Minimize;
  if (name == "maximize") return Icon::Maximize;
  if (name == "command" || name == "cmd") return Icon::Command;
  if (name == "check") return Icon::Check;
  if (name == "user" || name == "avatar") return Icon::User;
  if (name == "power") return Icon::Power;
  if (name == "reply") return Icon::Reply;
  if (name == "reply_all" || name == "replyall") return Icon::ReplyAll;
  if (name == "forward") return Icon::Forward;
  if (name == "flag") return Icon::Flag;
  if (name == "trash" || name == "delete") return Icon::Trash;
  if (name == "archive") return Icon::Archive;
  if (name == "refresh" || name == "sync") return Icon::Refresh;
  if (name == "copy") return Icon::Copy;
  if (name == "sun") return Icon::Sun;
  if (name == "moon") return Icon::Moon;
  if (name == "paperclip" || name == "attachment") return Icon::Paperclip;
  if (name == "plus" || name == "add") return Icon::Plus;
  if (name == "filter" || name == "funnel") return Icon::Filter;
  if (name == "download" || name == "export") return Icon::Download;
  if (name == "skipback" || name == "first") return Icon::SkipBack;
  if (name == "skipforward" || name == "last") return Icon::SkipForward;
  if (name == "columns") return Icon::Columns;
  if (name == "plug" || name == "connect") return Icon::Plug;
  return Icon::None;
}

void ButtonSuite::draw_icon(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                            int cx, int cy, Icon icon, int size,
                            uint32_t color, uint32_t secondary_color) {
  if (size <= 0) return;
  float fcx = static_cast<float>(cx);
  float fcy = static_cast<float>(cy);
  float s = static_cast<float>(size);
  float half = s * 0.5f;

  if (secondary_color == 0) secondary_color = color;

  switch (icon) {
    case Icon::Terminal: {
      // Chevron prompt > and cursor underscore _
      float left = fcx - half * 0.7f;
      float mid = fcx - half * 0.1f;
      float top = fcy - half * 0.55f;
      float bot = fcy + half * 0.15f;
      float line_w = std::max(1.6f, s * 0.09f);

      draw_line_aa(buf, buf_w, buf_h, left, top, mid, (top + bot) * 0.5f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, mid, (top + bot) * 0.5f, left, bot, color, line_w);

      // Underscore cursor
      float cur_x0 = fcx + half * 0.15f;
      float cur_x1 = fcx + half * 0.75f;
      float cur_y = fcy + half * 0.35f;
      draw_line_aa(buf, buf_w, buf_h, cur_x0, cur_y, cur_x1, cur_y, secondary_color, line_w * 1.1f);
      break;
    }
    case Icon::Code: {
      // Tags < / >
      float line_w = std::max(1.5f, s * 0.085f);
      float span = half * 0.8f;
      // <
      draw_line_aa(buf, buf_w, buf_h, fcx - span * 0.5f, fcy - span * 0.6f, fcx - span, fcy, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - span, fcy, fcx - span * 0.5f, fcy + span * 0.6f, color, line_w);
      // /
      draw_line_aa(buf, buf_w, buf_h, fcx + span * 0.2f, fcy - span * 0.7f, fcx - span * 0.2f, fcy + span * 0.7f, secondary_color, line_w);
      // >
      draw_line_aa(buf, buf_w, buf_h, fcx + span * 0.5f, fcy - span * 0.6f, fcx + span, fcy, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx + span, fcy, fcx + span * 0.5f, fcy + span * 0.6f, color, line_w);
      break;
    }
    case Icon::Database: {
      // 3D stacked database cylinders
      float rx = half * 0.8f;
      float ry = half * 0.3f;
      float disc_gap = half * 0.5f;
      float line_w = std::max(1.4f, s * 0.08f);

      // Top disc
      draw_circle(buf, buf_w, buf_h, fcx, fcy - disc_gap, rx, 0, color, line_w);
      // Middle curve
      draw_line_aa(buf, buf_w, buf_h, fcx - rx, fcy - disc_gap, fcx - rx, fcy + disc_gap, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx + rx, fcy - disc_gap, fcx + rx, fcy + disc_gap, color, line_w);

      // Horizontal curves for platters
      draw_line_aa(buf, buf_w, buf_h, fcx - rx, fcy, fcx, fcy + ry * 0.8f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx, fcy + ry * 0.8f, fcx + rx, fcy, color, line_w);

      draw_line_aa(buf, buf_w, buf_h, fcx - rx, fcy + disc_gap, fcx, fcy + disc_gap + ry * 0.8f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx, fcy + disc_gap + ry * 0.8f, fcx + rx, fcy + disc_gap, color, line_w);

      // Query lightning bolt / glow in center
      draw_circle(buf, buf_w, buf_h, fcx, fcy - disc_gap, rx * 0.4f, secondary_color, 0);
      break;
    }
    case Icon::Browser: {
      // Globe with latitude and longitude lines
      float r = half * 0.8f;
      float line_w = std::max(1.4f, s * 0.08f);
      draw_circle(buf, buf_w, buf_h, fcx, fcy, r, 0, color, line_w);

      // Equator
      draw_line_aa(buf, buf_w, buf_h, fcx - r, fcy, fcx + r, fcy, color, line_w);
      // Prime meridian line
      draw_line_aa(buf, buf_w, buf_h, fcx, fcy - r, fcx, fcy + r, color, line_w);

      // Meridian arc approximation
      draw_line_aa(buf, buf_w, buf_h, fcx, fcy - r, fcx - r * 0.5f, fcy, secondary_color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - r * 0.5f, fcy, fcx, fcy + r, secondary_color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx, fcy - r, fcx + r * 0.5f, fcy, secondary_color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx + r * 0.5f, fcy, fcx, fcy + r, secondary_color, line_w);
      break;
    }
    case Icon::Mail: {
      // Sealed envelope with crisp triangular flap
      float ew = half * 1.6f;
      float eh = half * 1.15f;
      float line_w = std::max(1.4f, s * 0.08f);
      float left = fcx - ew * 0.5f;
      float right = fcx + ew * 0.5f;
      float top = fcy - eh * 0.5f;
      float bot = fcy + eh * 0.5f;

      // Outer envelope box
      draw_line_aa(buf, buf_w, buf_h, left, top, right, top, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, right, top, right, bot, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, right, bot, left, bot, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, left, bot, left, top, color, line_w);

      // Flap crease to center
      draw_line_aa(buf, buf_w, buf_h, left, top, fcx, fcy + eh * 0.1f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, right, top, fcx, fcy + eh * 0.1f, color, line_w);

      // Seal dot
      draw_circle(buf, buf_w, buf_h, fcx, fcy + eh * 0.08f, std::max(1.8f, s * 0.09f), secondary_color, 0);
      break;
    }
    case Icon::Inspector:
    case Icon::Gear: {
      // Telemetry Equalizer Sliders
      float line_w = std::max(1.5f, s * 0.085f);
      float track_h = half * 1.4f;
      float top = fcy - track_h * 0.5f;
      float bot = fcy + track_h * 0.5f;
      float x0 = fcx - half * 0.6f;
      float x1 = fcx;
      float x2 = fcx + half * 0.6f;

      // 3 vertical tracks
      draw_line_aa(buf, buf_w, buf_h, x0, top, x0, bot, color, line_w * 0.8f);
      draw_line_aa(buf, buf_w, buf_h, x1, top, x1, bot, color, line_w * 0.8f);
      draw_line_aa(buf, buf_w, buf_h, x2, top, x2, bot, color, line_w * 0.8f);

      // Slider knobs
      float knob_r = std::max(2.2f, s * 0.12f);
      draw_circle(buf, buf_w, buf_h, x0, fcy - track_h * 0.2f, knob_r, secondary_color, color, 1.0f);
      draw_circle(buf, buf_w, buf_h, x1, fcy + track_h * 0.25f, knob_r, secondary_color, color, 1.0f);
      draw_circle(buf, buf_w, buf_h, x2, fcy - track_h * 0.1f, knob_r, secondary_color, color, 1.0f);
      break;
    }
    case Icon::Flame: {
      // Stylized dual-curve flame
      float line_w = std::max(1.5f, s * 0.085f);
      float tip_x = fcx;
      float tip_y = fcy - half * 0.85f;
      float base_y = fcy + half * 0.8f;
      float left_belly = fcx - half * 0.7f;
      float right_belly = fcx + half * 0.65f;

      // Outer contour
      draw_line_aa(buf, buf_w, buf_h, tip_x, tip_y, left_belly, fcy + half * 0.2f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, left_belly, fcy + half * 0.2f, fcx, base_y, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx, base_y, right_belly, fcy + half * 0.3f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, right_belly, fcy + half * 0.3f, tip_x, tip_y, color, line_w);

      // Inner flame core
      draw_circle(buf, buf_w, buf_h, fcx, fcy + half * 0.25f, half * 0.3f, secondary_color, 0);
      break;
    }
    case Icon::Box: {
      // Isometric 3D box cube
      float line_w = std::max(1.4f, s * 0.08f);
      float r = half * 0.8f;
      float y_mid = fcy;
      float y_top = fcy - r * 0.8f;
      float y_bot = fcy + r * 0.8f;
      float x_l = fcx - r * 0.9f;
      float x_r = fcx + r * 0.9f;

      // Top face
      draw_line_aa(buf, buf_w, buf_h, fcx, y_top, x_r, y_mid - r * 0.35f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, x_r, y_mid - r * 0.35f, fcx, y_mid + r * 0.1f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx, y_mid + r * 0.1f, x_l, y_mid - r * 0.35f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, x_l, y_mid - r * 0.35f, fcx, y_top, color, line_w);

      // Bottom edges
      draw_line_aa(buf, buf_w, buf_h, x_l, y_mid - r * 0.35f, x_l, y_bot - r * 0.35f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx, y_mid + r * 0.1f, fcx, y_bot + r * 0.1f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, x_r, y_mid - r * 0.35f, x_r, y_bot - r * 0.35f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, x_l, y_bot - r * 0.35f, fcx, y_bot + r * 0.1f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx, y_bot + r * 0.1f, x_r, y_bot - r * 0.35f, color, line_w);
      break;
    }
    case Icon::Sql: {
      // Data table grid with query bolt
      float line_w = std::max(1.3f, s * 0.075f);
      float tw = half * 1.5f;
      float th = half * 1.2f;
      float left = fcx - tw * 0.5f;
      float right = fcx + tw * 0.5f;
      float top = fcy - th * 0.5f;
      float bot = fcy + th * 0.5f;

      // Table outer
      draw_rounded_rect(buf, buf_w, buf_h, static_cast<int>(left), static_cast<int>(top), static_cast<int>(tw), static_cast<int>(th), 2, 0);
      draw_round_outline(buf, buf_w, buf_h, static_cast<int>(left), static_cast<int>(top), static_cast<int>(tw), static_cast<int>(th), 2, color, line_w);

      // Header line
      float header_y = top + th * 0.32f;
      draw_line_aa(buf, buf_w, buf_h, left, header_y, right, header_y, color, line_w);
      // Column line
      draw_line_aa(buf, buf_w, buf_h, fcx - tw * 0.1f, top, fcx - tw * 0.1f, bot, color, line_w);

      // Lightning accent
      draw_line_aa(buf, buf_w, buf_h, fcx + tw * 0.2f, top + th * 0.45f, fcx + tw * 0.05f, top + th * 0.7f, secondary_color, line_w * 1.2f);
      draw_line_aa(buf, buf_w, buf_h, fcx + tw * 0.05f, top + th * 0.7f, fcx + tw * 0.35f, top + th * 0.65f, secondary_color, line_w * 1.2f);
      draw_line_aa(buf, buf_w, buf_h, fcx + tw * 0.35f, top + th * 0.65f, fcx + tw * 0.15f, bot - 1.0f, secondary_color, line_w * 1.2f);
      break;
    }
    case Icon::Cluster: {
      // 4-node distributed mesh graph with status interconnects
      float r = half * 0.75f;
      float node_r = std::max(2.0f, s * 0.11f);
      float line_w = std::max(1.2f, s * 0.07f);

      float p_top_x = fcx, p_top_y = fcy - r;
      float p_bot_x = fcx, p_bot_y = fcy + r;
      float p_left_x = fcx - r, p_left_y = fcy;
      float p_right_x = fcx + r, p_right_y = fcy;

      // Interconnect links
      draw_line_aa(buf, buf_w, buf_h, p_top_x, p_top_y, p_left_x, p_left_y, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, p_top_x, p_top_y, p_right_x, p_right_y, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, p_bot_x, p_bot_y, p_left_x, p_left_y, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, p_bot_x, p_bot_y, p_right_x, p_right_y, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, p_left_x, p_left_y, p_right_x, p_right_y, color, line_w * 0.8f);

      // Node circles
      draw_circle(buf, buf_w, buf_h, p_top_x, p_top_y, node_r, secondary_color, color, 1.0f);
      draw_circle(buf, buf_w, buf_h, p_bot_x, p_bot_y, node_r, secondary_color, color, 1.0f);
      draw_circle(buf, buf_w, buf_h, p_left_x, p_left_y, node_r, secondary_color, color, 1.0f);
      draw_circle(buf, buf_w, buf_h, p_right_x, p_right_y, node_r, secondary_color, color, 1.0f);
      break;
    }
    case Icon::Search: {
      // 45-degree angled magnifying glass
      float lens_r = half * 0.45f;
      float lens_cx = fcx - half * 0.2f;
      float lens_cy = fcy - half * 0.2f;
      float line_w = std::max(1.6f, s * 0.09f);

      draw_circle(buf, buf_w, buf_h, lens_cx, lens_cy, lens_r, 0, color, line_w);
      // Handle
      float h_start_x = lens_cx + lens_r * 0.707f;
      float h_start_y = lens_cy + lens_r * 0.707f;
      float h_end_x = fcx + half * 0.75f;
      float h_end_y = fcy + half * 0.75f;
      draw_line_aa(buf, buf_w, buf_h, h_start_x, h_start_y, h_end_x, h_end_y, color, line_w * 1.3f);
      break;
    }
    case Icon::Play: {
      // Equilateral play triangle
      float p_r = half * 0.65f;
      float line_w = std::max(1.8f, s * 0.1f);
      float x0 = fcx - p_r * 0.6f, y0 = fcy - p_r;
      float x1 = fcx + p_r, y1 = fcy;
      float x2 = fcx - p_r * 0.6f, y2 = fcy + p_r;
      draw_line_aa(buf, buf_w, buf_h, x0, y0, x1, y1, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, x1, y1, x2, y2, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, x2, y2, x0, y0, color, line_w);
      break;
    }
    case Icon::Stop: {
      // Stop square
      float sw = half * 1.1f;
      draw_rounded_rect(buf, buf_w, buf_h, static_cast<int>(fcx - sw * 0.5f), static_cast<int>(fcy - sw * 0.5f),
                        static_cast<int>(sw), static_cast<int>(sw), 2, color);
      break;
    }
    case Icon::Close: {
      // Crisp diagonal cross
      float r = half * 0.65f;
      float line_w = std::max(1.6f, s * 0.09f);
      draw_line_aa(buf, buf_w, buf_h, fcx - r, fcy - r, fcx + r, fcy + r, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx + r, fcy - r, fcx - r, fcy + r, color, line_w);
      break;
    }
    case Icon::Minimize: {
      // Horizontal minus bar
      float r = half * 0.7f;
      float line_w = std::max(1.8f, s * 0.1f);
      draw_line_aa(buf, buf_w, buf_h, fcx - r, fcy, fcx + r, fcy, color, line_w);
      break;
    }
    case Icon::Maximize: {
      // Window frame rectangle
      float r = half * 0.65f;
      float line_w = std::max(1.4f, s * 0.08f);
      draw_round_outline(buf, buf_w, buf_h, static_cast<int>(fcx - r), static_cast<int>(fcy - r),
                         static_cast<int>(r * 2.0f), static_cast<int>(r * 2.0f), 2, color, line_w);
      break;
    }
    case Icon::Command: {
      // ⌘ loop symbol
      float r = half * 0.75f;
      float lr = r * 0.32f;
      float line_w = std::max(1.4f, s * 0.08f);
      float c = r * 0.4f;

      // Center square
      draw_line_aa(buf, buf_w, buf_h, fcx - c, fcy - c, fcx + c, fcy - c, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx + c, fcy - c, fcx + c, fcy + c, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx + c, fcy + c, fcx - c, fcy + c, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - c, fcy + c, fcx - c, fcy - c, color, line_w);

      // 4 corner loops
      draw_circle(buf, buf_w, buf_h, fcx - c, fcy - c, lr, 0, color, line_w);
      draw_circle(buf, buf_w, buf_h, fcx + c, fcy - c, lr, 0, color, line_w);
      draw_circle(buf, buf_w, buf_h, fcx - c, fcy + c, lr, 0, color, line_w);
      draw_circle(buf, buf_w, buf_h, fcx + c, fcy + c, lr, 0, color, line_w);
      break;
    }
    case Icon::Return: {
      // Carriage return arrow ↵
      float line_w = std::max(1.5f, s * 0.085f);
      float rx = fcx + half * 0.5f;
      float lx = fcx - half * 0.6f;
      float ty = fcy - half * 0.5f;
      float by = fcy + half * 0.2f;

      draw_line_aa(buf, buf_w, buf_h, rx, ty, rx, by, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, rx, by, lx, by, color, line_w);
      // Arrowhead
      draw_line_aa(buf, buf_w, buf_h, lx + half * 0.35f, by - half * 0.35f, lx, by, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, lx + half * 0.35f, by + half * 0.35f, lx, by, color, line_w);
      break;
    }
    case Icon::ArrowUp: {
      float line_w = std::max(1.5f, s * 0.085f);
      draw_line_aa(buf, buf_w, buf_h, fcx, fcy + half * 0.6f, fcx, fcy - half * 0.6f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - half * 0.45f, fcy - half * 0.15f, fcx, fcy - half * 0.6f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx + half * 0.45f, fcy - half * 0.15f, fcx, fcy - half * 0.6f, color, line_w);
      break;
    }
    case Icon::ArrowDown: {
      float line_w = std::max(1.5f, s * 0.085f);
      draw_line_aa(buf, buf_w, buf_h, fcx, fcy - half * 0.6f, fcx, fcy + half * 0.6f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - half * 0.45f, fcy + half * 0.15f, fcx, fcy + half * 0.6f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx + half * 0.45f, fcy + half * 0.15f, fcx, fcy + half * 0.6f, color, line_w);
      break;
    }
    // ArrowLeft and ArrowRight were declared in the enum and never drawn: every
    // caller asking for one got an empty button. A pager made of empty buttons
    // is how a grid ends up with no way to turn the page.
    case Icon::ArrowLeft: {
      float line_w = std::max(1.5f, s * 0.085f);
      draw_line_aa(buf, buf_w, buf_h, fcx + half * 0.6f, fcy, fcx - half * 0.6f, fcy, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - half * 0.15f, fcy - half * 0.45f, fcx - half * 0.6f, fcy, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - half * 0.15f, fcy + half * 0.45f, fcx - half * 0.6f, fcy, color, line_w);
      break;
    }
    case Icon::ArrowRight: {
      float line_w = std::max(1.5f, s * 0.085f);
      draw_line_aa(buf, buf_w, buf_h, fcx - half * 0.6f, fcy, fcx + half * 0.6f, fcy, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx + half * 0.15f, fcy - half * 0.45f, fcx + half * 0.6f, fcy, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx + half * 0.15f, fcy + half * 0.45f, fcx + half * 0.6f, fcy, color, line_w);
      break;
    }
    case Icon::Check: {
      float line_w = std::max(1.7f, s * 0.095f);
      draw_line_aa(buf, buf_w, buf_h, fcx - half * 0.6f, fcy, fcx - half * 0.1f, fcy + half * 0.5f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - half * 0.1f, fcy + half * 0.5f, fcx + half * 0.7f, fcy - half * 0.5f, color, line_w);
      break;
    }
    case Icon::Sparkle: {
      // 4-point diamond star
      float r = half * 0.8f;
      float line_w = std::max(1.6f, s * 0.09f);
      draw_line_aa(buf, buf_w, buf_h, fcx, fcy - r, fcx, fcy + r, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - r, fcy, fcx + r, fcy, color, line_w);
      draw_circle(buf, buf_w, buf_h, fcx, fcy, r * 0.3f, secondary_color, 0);
      break;
    }
    case Icon::User: {
      // Clean avatar head + shoulders silhouette
      float head_r = half * 0.38f;
      draw_circle(buf, buf_w, buf_h, fcx, fcy - half * 0.32f, head_r, color, 0);
      // Shoulders arc
      float sw = half * 0.85f;
      float sy = fcy + half * 0.5f;
      draw_line_aa(buf, buf_w, buf_h, fcx - sw, sy + half * 0.2f, fcx - sw * 0.5f, sy, color, 2.2f);
      draw_line_aa(buf, buf_w, buf_h, fcx - sw * 0.5f, sy, fcx + sw * 0.5f, sy, color, 2.2f);
      draw_line_aa(buf, buf_w, buf_h, fcx + sw * 0.5f, sy, fcx + sw, sy + half * 0.2f, color, 2.2f);
      break;
    }
    case Icon::Power: {
      float r = half * 0.75f;
      float line_w = std::max(1.6f, s * 0.09f);
      // Standby circle
      draw_circle(buf, buf_w, buf_h, fcx, fcy, r, 0, color, line_w);
      // Top cut-out background cover
      draw_rounded_rect(buf, buf_w, buf_h, static_cast<int>(fcx - 3), static_cast<int>(fcy - r - 2), 6, 6, 0, 0xFA0B0E18);
      // Vertical power stroke
      draw_line_aa(buf, buf_w, buf_h, fcx, fcy - r - 1.0f, fcx, fcy - 1.0f, color, line_w * 1.1f);
      break;
    }
    case Icon::Reply: {
      float line_w = std::max(1.5f, s * 0.085f);
      float start_x = fcx + half * 0.6f;
      float bend_x = fcx + half * 0.1f;
      float top_y = fcy - half * 0.5f;
      float bot_y = fcy + half * 0.4f;
      float tip_x = fcx - half * 0.65f;

      draw_line_aa(buf, buf_w, buf_h, start_x, bot_y, start_x, top_y + half * 0.25f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, start_x, top_y + half * 0.25f, bend_x, top_y, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, bend_x, top_y, tip_x, top_y, color, line_w);

      draw_line_aa(buf, buf_w, buf_h, tip_x + half * 0.35f, top_y - half * 0.35f, tip_x, top_y, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, tip_x + half * 0.35f, top_y + half * 0.35f, tip_x, top_y, color, line_w);
      break;
    }
    case Icon::ReplyAll: {
      float line_w = std::max(1.4f, s * 0.08f);
      float start_x = fcx + half * 0.7f;
      float bend_x = fcx + half * 0.2f;
      float top_y = fcy - half * 0.45f;
      float bot_y = fcy + half * 0.35f;
      float tip1_x = fcx - half * 0.15f;
      float tip2_x = fcx - half * 0.75f;

      draw_line_aa(buf, buf_w, buf_h, start_x, bot_y, start_x, top_y + half * 0.2f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, start_x, top_y + half * 0.2f, bend_x, top_y, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, bend_x, top_y, tip2_x, top_y, color, line_w);

      draw_line_aa(buf, buf_w, buf_h, tip2_x + half * 0.3f, top_y - half * 0.3f, tip2_x, top_y, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, tip2_x + half * 0.3f, top_y + half * 0.3f, tip2_x, top_y, color, line_w);

      draw_line_aa(buf, buf_w, buf_h, tip1_x + half * 0.3f, top_y - half * 0.3f, tip1_x, top_y, secondary_color, line_w);
      draw_line_aa(buf, buf_w, buf_h, tip1_x + half * 0.3f, top_y + half * 0.3f, tip1_x, top_y, secondary_color, line_w);
      break;
    }
    case Icon::Forward: {
      float line_w = std::max(1.5f, s * 0.085f);
      float start_x = fcx - half * 0.6f;
      float bend_x = fcx - half * 0.1f;
      float top_y = fcy - half * 0.5f;
      float bot_y = fcy + half * 0.4f;
      float tip_x = fcx + half * 0.65f;

      draw_line_aa(buf, buf_w, buf_h, start_x, bot_y, start_x, top_y + half * 0.25f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, start_x, top_y + half * 0.25f, bend_x, top_y, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, bend_x, top_y, tip_x, top_y, color, line_w);

      draw_line_aa(buf, buf_w, buf_h, tip_x - half * 0.35f, top_y - half * 0.35f, tip_x, top_y, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, tip_x - half * 0.35f, top_y + half * 0.35f, tip_x, top_y, color, line_w);
      break;
    }
    case Icon::Flag: {
      float line_w = std::max(1.5f, s * 0.085f);
      float pole_x = fcx - half * 0.55f;
      float top_y = fcy - half * 0.75f;
      float bot_y = fcy + half * 0.8f;
      float flag_w = half * 1.15f;
      float flag_h = half * 0.75f;

      draw_line_aa(buf, buf_w, buf_h, pole_x, top_y, pole_x, bot_y, color, line_w * 1.1f);
      draw_line_aa(buf, buf_w, buf_h, pole_x, top_y, pole_x + flag_w, top_y + flag_h * 0.45f, secondary_color, line_w);
      draw_line_aa(buf, buf_w, buf_h, pole_x + flag_w, top_y + flag_h * 0.45f, pole_x, top_y + flag_h, secondary_color, line_w);
      draw_line_aa(buf, buf_w, buf_h, pole_x, top_y + flag_h, pole_x, top_y, secondary_color, line_w);
      break;
    }
    case Icon::Trash: {
      float line_w = std::max(1.4f, s * 0.08f);
      float bw = half * 0.75f;
      float top_y = fcy - half * 0.45f;
      float bot_y = fcy + half * 0.7f;

      draw_line_aa(buf, buf_w, buf_h, fcx - bw * 1.2f, top_y, fcx + bw * 1.2f, top_y, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - bw * 0.4f, top_y - half * 0.25f, fcx + bw * 0.4f, top_y - half * 0.25f, color, line_w);

      draw_line_aa(buf, buf_w, buf_h, fcx - bw, top_y, fcx - bw * 0.8f, bot_y, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx + bw, top_y, fcx + bw * 0.8f, bot_y, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - bw * 0.8f, bot_y, fcx + bw * 0.8f, bot_y, color, line_w);

      draw_line_aa(buf, buf_w, buf_h, fcx - bw * 0.35f, top_y + half * 0.2f, fcx - bw * 0.25f, bot_y - half * 0.15f, color, line_w * 0.8f);
      draw_line_aa(buf, buf_w, buf_h, fcx + bw * 0.35f, top_y + half * 0.2f, fcx + bw * 0.25f, bot_y - half * 0.15f, color, line_w * 0.8f);
      break;
    }
    case Icon::Archive: {
      float line_w = std::max(1.4f, s * 0.08f);
      float box_w = half * 1.3f;
      float box_h = half * 1.0f;
      float left = fcx - box_w * 0.5f;
      float right = fcx + box_w * 0.5f;
      float top = fcy - box_h * 0.5f;
      float bot = fcy + box_h * 0.5f;
      float lid_y = top + box_h * 0.35f;

      draw_line_aa(buf, buf_w, buf_h, left - 1.0f, top, right + 1.0f, top, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, right + 1.0f, top, right + 1.0f, lid_y, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, right + 1.0f, lid_y, left - 1.0f, lid_y, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, left - 1.0f, lid_y, left - 1.0f, top, color, line_w);

      draw_line_aa(buf, buf_w, buf_h, left, lid_y, left, bot, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, left, bot, right, bot, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, right, bot, right, lid_y, color, line_w);

      draw_line_aa(buf, buf_w, buf_h, fcx - half * 0.25f, lid_y + half * 0.25f, fcx + half * 0.25f, lid_y + half * 0.25f, secondary_color, line_w);
      break;
    }
    case Icon::Refresh: {
      float r = half * 0.65f;
      float line_w = std::max(1.5f, s * 0.085f);
      draw_circle(buf, buf_w, buf_h, fcx, fcy, r, 0, color, line_w);
      draw_rounded_rect(buf, buf_w, buf_h, static_cast<int>(fcx - r * 0.35f), static_cast<int>(fcy - r - 3.0f), static_cast<int>(r * 0.95f), static_cast<int>(r * 0.75f), 0, 0);

      float ax = fcx + r * 0.55f;
      float ay = fcy - r * 0.7f;
      draw_line_aa(buf, buf_w, buf_h, ax - half * 0.35f, ay - half * 0.2f, ax, ay, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, ax - half * 0.35f, ay + half * 0.3f, ax, ay, color, line_w);
      break;
    }
    case Icon::Copy: {
      float line_w = std::max(1.3f, s * 0.075f);
      float sw = half * 0.8f;
      float sh = half * 1.0f;
      float off = half * 0.28f;

      draw_round_outline(buf, buf_w, buf_h, static_cast<int>(fcx - sw * 0.5f + off), static_cast<int>(fcy - sh * 0.5f - off), static_cast<int>(sw), static_cast<int>(sh), 2, secondary_color, line_w);
      draw_rounded_rect(buf, buf_w, buf_h, static_cast<int>(fcx - sw * 0.5f - off * 0.5f), static_cast<int>(fcy - sh * 0.5f + off * 0.5f), static_cast<int>(sw), static_cast<int>(sh), 2, 0);
      draw_round_outline(buf, buf_w, buf_h, static_cast<int>(fcx - sw * 0.5f - off * 0.5f), static_cast<int>(fcy - sh * 0.5f + off * 0.5f), static_cast<int>(sw), static_cast<int>(sh), 2, color, line_w);
      break;
    }
    case Icon::Sun: {
      float cr = half * 0.38f;
      float ray_in = half * 0.55f;
      float ray_out = half * 0.85f;
      float line_w = std::max(1.4f, s * 0.08f);

      draw_circle(buf, buf_w, buf_h, fcx, fcy, cr, color, 0);
      for (int a = 0; a < 8; ++a) {
        float rad = a * 3.14159265f / 4.0f;
        float cos_a = std::cos(rad), sin_a = std::sin(rad);
        draw_line_aa(buf, buf_w, buf_h, fcx + cos_a * ray_in, fcy + sin_a * ray_in, fcx + cos_a * ray_out, fcy + sin_a * ray_out, color, line_w);
      }
      break;
    }
    case Icon::Moon: {
      float r = half * 0.7f;
      draw_circle(buf, buf_w, buf_h, fcx, fcy, r, color, 0);
      draw_circle(buf, buf_w, buf_h, fcx + r * 0.45f, fcy - r * 0.25f, r * 0.85f, 0, 0);
      break;
    }
    case Icon::Paperclip: {
      float line_w = std::max(1.4f, s * 0.08f);
      float r = half * 0.35f;
      draw_line_aa(buf, buf_w, buf_h, fcx - r, fcy + half * 0.4f, fcx + r * 0.5f, fcy - half * 0.4f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - r * 0.4f, fcy + half * 0.6f, fcx + r * 0.9f, fcy - half * 0.2f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - r, fcy + half * 0.4f, fcx - r * 0.4f, fcy + half * 0.6f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx + r * 0.5f, fcy - half * 0.4f, fcx + r * 0.9f, fcy - half * 0.2f, color, line_w);
      break;
    }
    case Icon::Plus: {
      float r = half * 0.62f;
      float line_w = std::max(1.7f, s * 0.1f);
      draw_line_aa(buf, buf_w, buf_h, fcx - r, fcy, fcx + r, fcy, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx, fcy - r, fcx, fcy + r, color, line_w);
      break;
    }
    case Icon::Filter: {
      // A funnel: wide mouth, converging walls, a stem below.
      float line_w = std::max(1.4f, s * 0.082f);
      float top = fcy - half * 0.62f;
      float wide = half * 0.72f;
      draw_line_aa(buf, buf_w, buf_h, fcx - wide, top, fcx + wide, top, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - wide, top, fcx - half * 0.16f, fcy + half * 0.12f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx + wide, top, fcx + half * 0.16f, fcy + half * 0.12f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - half * 0.16f, fcy + half * 0.12f, fcx - half * 0.16f, fcy + half * 0.66f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx + half * 0.16f, fcy + half * 0.12f, fcx + half * 0.16f, fcy + half * 0.45f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - half * 0.16f, fcy + half * 0.66f, fcx + half * 0.16f, fcy + half * 0.45f, color, line_w);
      break;
    }
    case Icon::Download: {
      float line_w = std::max(1.5f, s * 0.085f);
      float r = half * 0.55f;
      draw_line_aa(buf, buf_w, buf_h, fcx, fcy - half * 0.75f, fcx, fcy + half * 0.18f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - r * 0.65f, fcy - half * 0.22f, fcx, fcy + half * 0.18f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx + r * 0.65f, fcy - half * 0.22f, fcx, fcy + half * 0.18f, color, line_w);
      // the tray it drops into
      draw_line_aa(buf, buf_w, buf_h, fcx - r, fcy + half * 0.5f, fcx + r, fcy + half * 0.5f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - r, fcy + half * 0.2f, fcx - r, fcy + half * 0.5f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx + r, fcy + half * 0.2f, fcx + r, fcy + half * 0.5f, color, line_w);
      break;
    }
    case Icon::SkipBack: {
      float line_w = std::max(1.6f, s * 0.09f);
      float r = half * 0.6f;
      draw_line_aa(buf, buf_w, buf_h, fcx - r, fcy - r, fcx - r, fcy + r, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx + r, fcy - r, fcx - r * 0.35f, fcy, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx + r, fcy + r, fcx - r * 0.35f, fcy, color, line_w);
      break;
    }
    case Icon::SkipForward: {
      float line_w = std::max(1.6f, s * 0.09f);
      float r = half * 0.6f;
      draw_line_aa(buf, buf_w, buf_h, fcx + r, fcy - r, fcx + r, fcy + r, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - r, fcy - r, fcx + r * 0.35f, fcy, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - r, fcy + r, fcx + r * 0.35f, fcy, color, line_w);
      break;
    }
    case Icon::Columns: {
      float line_w = std::max(1.3f, s * 0.075f);
      float r = half * 0.68f;
      draw_line_aa(buf, buf_w, buf_h, fcx - r, fcy - r, fcx + r, fcy - r, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - r, fcy + r, fcx + r, fcy + r, color, line_w);
      for (int i = 0; i < 3; ++i) {
        float x = fcx - r + (2.0f * r) * (float)i / 2.0f;
        draw_line_aa(buf, buf_w, buf_h, x, fcy - r, x, fcy + r, color, line_w);
      }
      break;
    }
    case Icon::Plug: {
      float line_w = std::max(1.5f, s * 0.085f);
      float r = half * 0.5f;
      draw_circle(buf, buf_w, buf_h, fcx + half * 0.12f, fcy, r, 0, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - half * 0.85f, fcy - r * 0.55f, fcx - half * 0.28f, fcy - r * 0.55f, color, line_w);
      draw_line_aa(buf, buf_w, buf_h, fcx - half * 0.85f, fcy + r * 0.55f, fcx - half * 0.28f, fcy + r * 0.55f, color, line_w);
      break;
    }
    default:
      break;
  }
}

void ButtonSuite::draw_dock_tile(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                                 int x, int y, int size,
                                 Icon icon, uint32_t base_color,
                                 bool hot, bool active,
                                 int active_pip_count) {
  int r = 10;
  // Dynamic gradient based on base_color
  float br, bg, bb;
  unpack_rgb(base_color, br, bg, bb);

  // Top specular tint
  uint32_t top_c = hot ? pack_rgb(br * 0.35f + 30.0f, bg * 0.35f + 35.0f, bb * 0.35f + 50.0f)
                       : pack_rgb(br * 0.22f + 18.0f, bg * 0.22f + 22.0f, bb * 0.22f + 32.0f);
  // Bottom darker shade
  uint32_t bot_c = hot ? pack_rgb(br * 0.18f + 14.0f, bg * 0.18f + 18.0f, bb * 0.18f + 25.0f)
                       : pack_rgb(br * 0.10f + 10.0f, bg * 0.10f + 12.0f, bb * 0.10f + 18.0f);

  // Rounded, like the outline stroked over it. This was a SQUARE gradient with
  // a rounded outline on top, so all four corners kept their fill outside the
  // stroke — invisible on a dark tile against a dark strip, and a hard square
  // corner on a bright one.
  draw_rounded_gradient_v(buf, buf_w, buf_h, x, y, size, size, r, top_c, bot_c);

  // Border outline: subtle ambient edge or bright hover accent
  uint32_t edge_c = hot ? pack_rgb(br * 0.8f + 60.0f, bg * 0.8f + 60.0f, bb * 0.8f + 60.0f)
                        : pack_rgb(br * 0.35f + 30.0f, bg * 0.35f + 35.0f, bb * 0.35f + 45.0f);
  draw_round_outline(buf, buf_w, buf_h, x, y, size, size, r, edge_c, hot ? 1.5f : 1.0f);

  // A 1px rim light along the top edge. This was a straight chord from x+r to
  // x+size-r — narrower than the rounded top actually is at that row, and cut
  // off flat at both ends — drawn with draw_line_aa in "0x40FFFFFF". That alpha
  // was never applied: blend_pixel reads its opacity from the argument, and the
  // line hands it the geometric coverage, so the intended 25% white was painted
  // as opaque white across two pixel rows. A white bar, not a rim light.
  draw_rounded_top_rim(buf, buf_w, buf_h, x, y, size, size, r, 0xFFFFFFFF, 0x40);

  // Centered Vector Icon
  int icon_s = static_cast<int>(size * 0.55f);
  uint32_t icon_c = hot ? 0xFFFFFFFF : pack_rgb(br * 0.9f + 50.0f, bg * 0.9f + 50.0f, bb * 0.9f + 50.0f);
  uint32_t icon_sec = hot ? base_color : pack_rgb(br * 0.75f, bg * 0.75f, bb * 0.75f);
  draw_icon(buf, buf_w, buf_h, x + size / 2, y + size / 2, icon, icon_s, icon_c, icon_sec);

  // Active indicator pip below tile
  if (active && active_pip_count > 0) {
    int pip_y = y + size + 5;
    int pip_w = 4;
    int pip_h = 4;
    int pip_x = x + (size - pip_w) / 2;
    draw_rounded_rect(buf, buf_w, buf_h, pip_x, pip_y, pip_w, pip_h, 2, 0xFFCBD5E1);
  }
}

namespace { constexpr int kKeycapRadius = 5; }

void ButtonSuite::draw_keycap(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                              int x, int y, int w, int h,
                              const std::string& label,
                              bool hot,
                              Icon icon) {
  // Beveled tactile keycap
  uint32_t bg_top = hot ? 0xFF243044 : 0xFF182232;
  uint32_t bg_bot = hot ? 0xFF182030 : 0xFF101622;
  draw_rounded_gradient_v(buf, buf_w, buf_h, x, y, w, h, kKeycapRadius, bg_top, bg_bot);

  // Bottom edge shadow for 3D bevel
  blend_pixel(buf, buf_w, buf_h, x, y + h - 1, 0xFF0A0E16, 220);
  draw_line_aa(buf, buf_w, buf_h, x + 2, y + h - 0.5f, x + w - 2, y + h - 0.5f, 0xFF080C14, 1.0f);

  // Specular top edge
  draw_line_aa(buf, buf_w, buf_h, x + 2, y + 0.5f, x + w - 2, y + 0.5f, hot ? 0xFF4B6082 : 0xFF2E3D52, 1.0f);
  // Rounded outline
  draw_round_outline(buf, buf_w, buf_h, x, y, w, h, kKeycapRadius, hot ? 0xFF3B82F6 : 0xFF26344A, 1.0f);

  bool has_cmd = (label.find("\xe2\x8c\x98") != std::string::npos);
  if (has_cmd) {
    std::string clean = label;
    size_t p = clean.find("\xe2\x8c\x98");
    if (p != std::string::npos) clean.erase(p, 3);
    while (!clean.empty() && clean[0] == ' ') clean.erase(0, 1);

    int cmd_s = 10;
    int tw = text_width_simple(clean, fonts::UI_SMALL);
    int total_w = cmd_s + 4 + tw;
    int start_x = x + (w - total_w) / 2;
    draw_icon(buf, buf_w, buf_h, start_x + cmd_s / 2, y + h / 2, Icon::Command, cmd_s, hot ? 0xFFFFFFFF : 0xFF94A3B8);
    int ty = y + (h - fonts::UI_SMALL.line_h) / 2 + 1;
    draw_text_simple(buf, buf_w, buf_h, start_x + cmd_s + 4, ty, clean, hot ? 0xFFFFFFFF : 0xFFCBD5E1, fonts::UI_SMALL);
    return;
  }

  int cur_x = x + 6;
  if (icon != Icon::None) {
    int ic_size = std::min(12, h - 8);
    draw_icon(buf, buf_w, buf_h, x + 10, y + h / 2, icon, ic_size, hot ? 0xFFFFFFFF : 0xFF94A3B8);
    cur_x += ic_size + 4;
  }

  if (!label.empty()) {
    int tw = text_width_simple(label, fonts::UI_SMALL);
    int tx = (icon == Icon::None) ? x + (w - tw) / 2 : cur_x;
    int ty = y + (h - fonts::UI_SMALL.line_h) / 2 + 1;
    draw_text_simple(buf, buf_w, buf_h, tx, ty, label, hot ? 0xFFFFFFFF : 0xFFCBD5E1, fonts::UI_SMALL);
  }
}

void ButtonSuite::draw_badge_button(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                                    int x, int y, int w, int h,
                                    const std::string& text,
                                    Icon icon,
                                    uint32_t bg_color,
                                    uint32_t fg_color,
                                    bool hot) {
  int r = 6;
  float br, bg, bb;
  unpack_rgb(bg_color, br, bg, bb);

  uint32_t top_c = hot ? pack_rgb(br * 1.3f + 15.0f, bg * 1.3f + 15.0f, bb * 1.3f + 15.0f) : bg_color;
  uint32_t bot_c = hot ? bg_color : pack_rgb(br * 0.75f, bg * 0.75f, bb * 0.75f);
  draw_rounded_gradient_v(buf, buf_w, buf_h, x, y, w, h, r, top_c, bot_c);

  uint32_t edge_c = hot ? fg_color : pack_rgb(br * 1.2f, bg * 1.2f, bb * 1.2f);
  draw_round_outline(buf, buf_w, buf_h, x, y, w, h, r, edge_c, 1.0f);

  if (icon != Icon::None) {
    int ic_size = std::min(14, h - 8);
    draw_icon(buf, buf_w, buf_h, x + w / 2, y + h / 2, icon, ic_size, fg_color, 0xFFFFFFFF);
  } else if (!text.empty()) {
    int tw = text_width_simple(text, fonts::UI_BOLD);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - fonts::UI_BOLD.line_h) / 2;
    draw_text_simple(buf, buf_w, buf_h, tx, ty, text, fg_color, fonts::UI_BOLD);
  }
}

void ButtonSuite::draw_button(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                              int x, int y, int w, int h,
                              const std::string& label,
                              Icon icon,
                              ButtonVariant variant,
                              ButtonState state) {
  bool hot = (state == ButtonState::Hover);
  bool pressed = (state == ButtonState::Pressed);
  int r = (variant == ButtonVariant::Pill) ? h / 2 : 6;

  uint32_t bg_top = 0xFF1E2838, bg_bot = 0xFF141A26;
  uint32_t edge_c = 0xFF2F3E56;
  uint32_t text_c = 0xFFCBD5E1;

  switch (variant) {
    case ButtonVariant::Primary:
      bg_top = hot ? 0xFF3B82F6 : 0xFF2563EB;
      bg_bot = hot ? 0xFF1D4ED8 : 0xFF1E40AF;
      edge_c = hot ? 0xFF60A5FA : 0xFF3B82F6;
      text_c = 0xFFFFFFFF;
      break;
    case ButtonVariant::Danger:
      bg_top = hot ? 0xFFEF4444 : 0xFFDC2626;
      bg_bot = hot ? 0xFFB91C1C : 0xFF991B1B;
      edge_c = hot ? 0xFFF87171 : 0xFFEF4444;
      text_c = 0xFFFFFFFF;
      break;
    case ButtonVariant::Ghost:
      if (!hot && !pressed) {
        bg_top = bg_bot = edge_c = 0;
      } else {
        bg_top = bg_bot = 0x25FFFFFF;
        edge_c = 0x40FFFFFF;
      }
      text_c = hot ? 0xFFFFFFFF : 0xFF94A3B8;
      break;
    default:
      if (hot) {
        bg_top = 0xFF28364C;
        bg_bot = 0xFF1C2638;
        edge_c = 0xFF3B82F6;
        text_c = 0xFFFFFFFF;
      }
      break;
  }

  if (bg_top != 0) {
    draw_rounded_gradient_v(buf, buf_w, buf_h, x, y, w, h, r, bg_top, bg_bot);
  }
  if (edge_c != 0) {
    draw_round_outline(buf, buf_w, buf_h, x, y, w, h, r, edge_c, 1.0f);
  }

  // Draw content (Icon + Label)
  int content_w = 0;
  int icon_s = 14;
  if (icon != Icon::None) content_w += icon_s + 6;
  if (!label.empty()) content_w += text_width_simple(label, fonts::UI);

  int cur_x = x + (w - content_w) / 2;
  if (icon != Icon::None) {
    draw_icon(buf, buf_w, buf_h, cur_x + icon_s / 2, y + h / 2, icon, icon_s, text_c);
    cur_x += icon_s + 6;
  }
  if (!label.empty()) {
    int ty = y + (h - fonts::UI.line_h) / 2;
    draw_text_simple(buf, buf_w, buf_h, cur_x, ty, label, text_c, fonts::UI);
  }
}

void ButtonSuite::draw_traffic_light(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                                     int cx, int cy, int size,
                                     TrafficLightType type,
                                     bool hot) {
  float r = size * 0.5f;
  uint32_t fill = 0;
  uint32_t edge = 0;
  Icon symbol = Icon::None;
  uint32_t sym_c = 0;

  switch (type) {
    case TrafficLightType::Close:
      fill = hot ? 0xFFFF5F56 : 0xFFE0443E;
      edge = 0xFFFF5F56;
      symbol = Icon::Close;
      sym_c = 0xFF4C0000;
      break;
    case TrafficLightType::Minimize:
      fill = hot ? 0xFFFFBD2E : 0xFFDEA123;
      edge = 0xFFFFBD2E;
      symbol = Icon::Minimize;
      sym_c = 0xFF5C4000;
      break;
    case TrafficLightType::Maximize:
      fill = hot ? 0xFF27C93F : 0xFF1AAB29;
      edge = 0xFF27C93F;
      symbol = Icon::Maximize;
      sym_c = 0xFF0D4012;
      break;
  }

  draw_circle(buf, buf_w, buf_h, static_cast<float>(cx), static_cast<float>(cy), r, fill, edge, 1.0f);

  if (hot) {
    draw_icon(buf, buf_w, buf_h, cx, cy, symbol, static_cast<int>(size * 0.65f), sym_c);
  }
}

void ButtonSuite::draw_window_action(std::vector<uint32_t>& buf, int buf_w, int buf_h,
                                     int x, int y, int w, int h,
                                     WindowAction action,
                                     bool hot) {
  int r = 4;
  bool danger = (action == WindowAction::Close);
  if (hot) {
    draw_rounded_rect(buf, buf_w, buf_h, x, y, w, h, r, danger ? 0xFF2A1414 : 0xFF1F293D);
    draw_round_outline(buf, buf_w, buf_h, x, y, w, h, r, danger ? 0xFFEF4444 : 0xFF3B82F6, 1.0f);
  }

  uint32_t sym_c = hot ? (danger ? 0xFFEF4444 : 0xFFFFFFFF) : 0xFF8B949E;
  int cx = x + w / 2;
  int cy = y + h / 2;
  int ic_s = 10;

  switch (action) {
    case WindowAction::Minimize:
      draw_icon(buf, buf_w, buf_h, cx, cy, Icon::Minimize, ic_s, sym_c);
      break;
    case WindowAction::Maximize:
      draw_icon(buf, buf_w, buf_h, cx, cy, Icon::Maximize, ic_s, sym_c);
      break;
    case WindowAction::Close:
      draw_icon(buf, buf_w, buf_h, cx, cy, Icon::Close, ic_s, sym_c);
      break;
  }
}

}  // namespace aios::userland
