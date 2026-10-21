// Canvas 2D, ImageData, Image, and Audio. The canvas draws into an ARGB buffer the pane puts on the glass (the
// document carries it as an inline image keyed by the element), so what a script draws is what is shown.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "aios/core/base64.hpp"
#include "aios/hal/audio.hpp"
#include "page.hpp"

namespace aios::userland::webjs {

using js::Tracer;

namespace {

// ---------------------------------------------------------------------------------------------------
// colours
// ---------------------------------------------------------------------------------------------------

struct Rgba {
  double r = 0, g = 0, b = 0, a = 1;   // 0..255, 0..255, 0..255, 0..1
};

const char* const kNamed =
    "aliceblue:f0f8ff antiquewhite:faebd7 aqua:00ffff aquamarine:7fffd4 azure:f0ffff beige:f5f5dc bisque:ffe4c4 black:000000 blanchedalmond:ffebcd blue:0000ff "
    "blueviolet:8a2be2 brown:a52a2a burlywood:deb887 cadetblue:5f9ea0 chartreuse:7fff00 chocolate:d2691e coral:ff7f50 cornflowerblue:6495ed cornsilk:fff8dc "
    "crimson:dc143c cyan:00ffff darkblue:00008b darkcyan:008b8b darkgoldenrod:b8860b darkgray:a9a9a9 darkgreen:006400 darkgrey:a9a9a9 darkkhaki:bdb76b "
    "darkmagenta:8b008b darkolivegreen:556b2f darkorange:ff8c00 darkorchid:9932cc darkred:8b0000 darksalmon:e9967a darkseagreen:8fbc8f darkslateblue:483d8b "
    "darkslategray:2f4f4f darkslategrey:2f4f4f darkturquoise:00ced1 darkviolet:9400d3 deeppink:ff1493 deepskyblue:00bfff dimgray:696969 dimgrey:696969 "
    "dodgerblue:1e90ff firebrick:b22222 floralwhite:fffaf0 forestgreen:228b22 fuchsia:ff00ff gainsboro:dcdcdc ghostwhite:f8f8ff gold:ffd700 goldenrod:daa520 "
    "gray:808080 green:008000 greenyellow:adff2f grey:808080 honeydew:f0fff0 hotpink:ff69b4 indianred:cd5c5c indigo:4b0082 ivory:fffff0 khaki:f0e68c "
    "lavender:e6e6fa lavenderblush:fff0f5 lawngreen:7cfc00 lemonchiffon:fffacd lightblue:add8e6 lightcoral:f08080 lightcyan:e0ffff lightgoldenrodyellow:fafad2 "
    "lightgray:d3d3d3 lightgreen:90ee90 lightgrey:d3d3d3 lightpink:ffb6c1 lightsalmon:ffa07a lightseagreen:20b2aa lightskyblue:87cefa lightslategray:778899 "
    "lightslategrey:778899 lightsteelblue:b0c4de lightyellow:ffffe0 lime:00ff00 limegreen:32cd32 linen:faf0e6 magenta:ff00ff maroon:800000 mediumaquamarine:66cdaa "
    "mediumblue:0000cd mediumorchid:ba55d3 mediumpurple:9370db mediumseagreen:3cb371 mediumslateblue:7b68ee mediumspringgreen:00fa9a mediumturquoise:48d1cc "
    "mediumvioletred:c71585 midnightblue:191970 mintcream:f5fffa mistyrose:ffe4e1 moccasin:ffe4b5 navajowhite:ffdead navy:000080 oldlace:fdf5e6 olive:808000 "
    "olivedrab:6b8e23 orange:ffa500 orangered:ff4500 orchid:da70d6 palegoldenrod:eee8aa palegreen:98fb98 paleturquoise:afeeee palevioletred:db7093 "
    "papayawhip:ffefd5 peachpuff:ffdab9 peru:cd853f pink:ffc0cb plum:dda0dd powderblue:b0e0e6 purple:800080 rebeccapurple:663399 red:ff0000 rosybrown:bc8f8f "
    "royalblue:4169e1 saddlebrown:8b4513 salmon:fa8072 sandybrown:f4a460 seagreen:2e8b57 seashell:fff5ee sienna:a0522d silver:c0c0c0 skyblue:87ceeb "
    "slateblue:6a5acd slategray:708090 slategrey:708090 snow:fffafa springgreen:00ff7f steelblue:4682b4 tan:d2b48c teal:008080 thistle:d8bfd8 tomato:ff6347 "
    "turquoise:40e0d0 violet:ee82ee wheat:f5deb3 white:ffffff whitesmoke:f5f5f5 yellow:ffff00 yellowgreen:9acd32";

int hexd(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

double hue_channel(double p, double q, double t) {
  if (t < 0) t += 1;
  if (t > 1) t -= 1;
  if (t < 1.0 / 6) return p + (q - p) * 6 * t;
  if (t < 0.5) return q;
  if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
  return p;
}

bool parse_number_list(const std::string& body, std::vector<double>* out, std::vector<bool>* pct) {
  std::string tok;
  auto flush = [&]() {
    if (tok.empty()) return true;
    bool p = tok.back() == '%';
    if (p) tok.pop_back();
    char* end = nullptr;
    double v = std::strtod(tok.c_str(), &end);
    if (end == tok.c_str()) return false;
    std::string unit(end);
    if (unit == "deg" || unit.empty() || unit == "turn" || unit == "rad" || unit == "grad") {
      if (unit == "turn") v *= 360;
      else if (unit == "rad") v *= 180.0 / 3.14159265358979;
      else if (unit == "grad") v *= 0.9;
    } else {
      return false;
    }
    out->push_back(v);
    pct->push_back(p);
    tok.clear();
    return true;
  };
  for (char c : body) {
    if (c == ',' || c == '/' || std::isspace(static_cast<unsigned char>(c))) { if (!flush()) return false; }
    else tok += c;
  }
  return flush();
}

bool parse_css_color(const std::string& in, Rgba* out) {
  std::string s;
  for (char c : in) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  s = s.substr(b);
  if (s.empty()) return false;
  if (s[0] == '#') {
    std::string h = s.substr(1);
    for (char c : h) if (hexd(c) < 0) return false;
    if (h.size() == 3 || h.size() == 4) {
      out->r = hexd(h[0]) * 17; out->g = hexd(h[1]) * 17; out->b = hexd(h[2]) * 17;
      out->a = h.size() == 4 ? hexd(h[3]) * 17 / 255.0 : 1;
      return true;
    }
    if (h.size() == 6 || h.size() == 8) {
      out->r = hexd(h[0]) * 16 + hexd(h[1]); out->g = hexd(h[2]) * 16 + hexd(h[3]); out->b = hexd(h[4]) * 16 + hexd(h[5]);
      out->a = h.size() == 8 ? (hexd(h[6]) * 16 + hexd(h[7])) / 255.0 : 1;
      return true;
    }
    return false;
  }
  const size_t paren = s.find('(');
  if (paren != std::string::npos && s.back() == ')') {
    const std::string fn = s.substr(0, paren), body = s.substr(paren + 1, s.size() - paren - 2);
    std::vector<double> v;
    std::vector<bool> pct;
    if (!parse_number_list(body, &v, &pct) || v.size() < 3) return false;
    const double alpha = v.size() > 3 ? (pct[3] ? v[3] / 100 : v[3]) : 1;
    if (fn == "rgb" || fn == "rgba") {
      auto ch = [&](size_t i) { return std::clamp(pct[i] ? v[i] * 2.55 : v[i], 0.0, 255.0); };
      out->r = ch(0); out->g = ch(1); out->b = ch(2);
      out->a = std::clamp(alpha, 0.0, 1.0);
      return true;
    }
    if (fn == "hsl" || fn == "hsla") {
      double h = std::fmod(v[0], 360) / 360, sat = std::clamp(v[1] / 100, 0.0, 1.0), l = std::clamp(v[2] / 100, 0.0, 1.0);
      if (h < 0) h += 1;
      double r, g, bl;
      if (sat == 0) r = g = bl = l;
      else {
        const double q = l < 0.5 ? l * (1 + sat) : l + sat - l * sat, p = 2 * l - q;
        r = hue_channel(p, q, h + 1.0 / 3);
        g = hue_channel(p, q, h);
        bl = hue_channel(p, q, h - 1.0 / 3);
      }
      out->r = r * 255; out->g = g * 255; out->b = bl * 255;
      out->a = std::clamp(alpha, 0.0, 1.0);
      return true;
    }
    return false;
  }
  if (s == "transparent") { *out = Rgba{0, 0, 0, 0}; return true; }
  if (s == "currentcolor") { *out = Rgba{0, 0, 0, 1}; return true; }
  const std::string key = s + ":";
  const char* at = std::strstr(kNamed, key.c_str());
  while (at && at != kNamed && at[-1] != ' ') at = std::strstr(at + 1, key.c_str());
  if (!at) return false;
  const char* hex = at + key.size();
  out->r = hexd(hex[0]) * 16 + hexd(hex[1]);
  out->g = hexd(hex[2]) * 16 + hexd(hex[3]);
  out->b = hexd(hex[4]) * 16 + hexd(hex[5]);
  out->a = 1;
  return true;
}

// ---------------------------------------------------------------------------------------------------
// the bitmap font (5x7, drawn on a 6x8 cell)
// ---------------------------------------------------------------------------------------------------

const uint8_t kFont[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12},
    {0x23, 0x13, 0x08, 0x64, 0x62}, {0x36, 0x49, 0x56, 0x20, 0x50}, {0x00, 0x08, 0x07, 0x03, 0x00}, {0x00, 0x1C, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1C, 0x00},
    {0x2A, 0x1C, 0x7F, 0x1C, 0x2A}, {0x08, 0x08, 0x3E, 0x08, 0x08}, {0x00, 0x80, 0x70, 0x30, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x00, 0x60, 0x60, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, {0x72, 0x49, 0x49, 0x49, 0x46}, {0x21, 0x41, 0x49, 0x4D, 0x33},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x31}, {0x41, 0x21, 0x11, 0x09, 0x07}, {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x46, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x00, 0x14, 0x00, 0x00}, {0x00, 0x40, 0x34, 0x00, 0x00}, {0x00, 0x08, 0x14, 0x22, 0x41}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x59, 0x09, 0x06}, {0x3E, 0x41, 0x5D, 0x59, 0x4E}, {0x7C, 0x12, 0x11, 0x12, 0x7C}, {0x7F, 0x49, 0x49, 0x49, 0x36},
    {0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01}, {0x3E, 0x41, 0x41, 0x51, 0x73},
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x1C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E},
    {0x7F, 0x09, 0x19, 0x29, 0x46}, {0x26, 0x49, 0x49, 0x49, 0x32}, {0x03, 0x01, 0x7F, 0x01, 0x03}, {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F},
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63}, {0x03, 0x04, 0x78, 0x04, 0x03}, {0x61, 0x59, 0x49, 0x4D, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x41},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x41, 0x7F}, {0x04, 0x02, 0x01, 0x02, 0x04}, {0x40, 0x40, 0x40, 0x40, 0x40}, {0x00, 0x03, 0x07, 0x08, 0x00},
    {0x20, 0x54, 0x54, 0x78, 0x40}, {0x7F, 0x28, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x28}, {0x38, 0x44, 0x44, 0x28, 0x7F}, {0x38, 0x54, 0x54, 0x54, 0x18},
    {0x00, 0x08, 0x7E, 0x09, 0x02}, {0x18, 0xA4, 0xA4, 0x9C, 0x78}, {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x20, 0x40, 0x40, 0x3D, 0x00},
    {0x7F, 0x10, 0x28, 0x44, 0x00}, {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x78, 0x04, 0x78}, {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38},
    {0xFC, 0x18, 0x24, 0x24, 0x18}, {0x18, 0x24, 0x24, 0x18, 0xFC}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x24}, {0x04, 0x04, 0x3F, 0x44, 0x24},
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C}, {0x3C, 0x40, 0x30, 0x40, 0x3C}, {0x44, 0x28, 0x10, 0x28, 0x44}, {0x4C, 0x90, 0x90, 0x90, 0x7C},
    {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00}, {0x00, 0x00, 0x77, 0x00, 0x00}, {0x00, 0x41, 0x36, 0x08, 0x00}, {0x02, 0x01, 0x02, 0x04, 0x02},
};

// ---------------------------------------------------------------------------------------------------
// the canvas state
// ---------------------------------------------------------------------------------------------------

struct Pt {
  double x = 0, y = 0;
};

struct Matrix {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  Pt apply(double x, double y) const { return {a * x + c * y + e, b * x + d * y + f}; }
  Matrix operator*(const Matrix& m) const {   // this applied after m: result(p) = this(m(p))
    return {a * m.a + c * m.b, b * m.a + d * m.b, a * m.c + c * m.d, b * m.c + d * m.d, a * m.e + c * m.f + e, b * m.e + d * m.f + f};
  }
  bool invert(Matrix* out) const {
    const double det = a * d - b * c;
    if (std::fabs(det) < 1e-12) return false;
    out->a = d / det; out->b = -b / det; out->c = -c / det; out->d = a / det;
    out->e = (c * f - d * e) / det; out->f = (b * e - a * f) / det;
    return true;
  }
  double scale_factor() const { return std::sqrt(std::fabs(a * d - b * c)); }
};

struct GradientData {
  bool radial = false;
  double x0 = 0, y0 = 0, r0 = 0, x1 = 0, y1 = 0, r1 = 0;
  std::vector<std::pair<double, Rgba>> stops;
};

struct GradientObject final : HostObject {
  GradientObject() : HostObject(HostKind::Gradient), data(std::make_shared<GradientData>()) {}
  std::shared_ptr<GradientData> data;
};

struct Paint {
  Rgba color{0, 0, 0, 1};
  std::shared_ptr<GradientData> gradient;   // shared with the CanvasGradient, as a browser's is
};

struct Sub {
  std::vector<Pt> pts;
  bool closed = false;
};

struct State {
  Matrix ctm;
  Paint fill, stroke;
  double line_width = 1, global_alpha = 1, miter_limit = 10;
  std::string line_cap = "butt", line_join = "miter", font = "10px sans-serif", text_align = "start", text_baseline = "alphabetic", composite = "source-over";
  std::vector<uint8_t> clip;   // empty: no clip
  std::vector<double> dash;
  double dash_offset = 0;
  std::string fill_style_text = "#000000", stroke_style_text = "#000000";
};

struct Canvas {
  int w = 300, h = 150;
  std::vector<uint32_t> pixels;
  State st;
  std::vector<State> stack;
  std::vector<Sub> path;
  bool dirty = false;
  Node* owner = nullptr;

  void resize(int nw, int nh) {
    if (nw <= 0) nw = 300;
    if (nh <= 0) nh = 150;
    if (nw > 8192) nw = 8192;
    if (nh > 8192) nh = 8192;
    if (nw == w && nh == h && pixels.size() == static_cast<size_t>(w) * h) return;
    w = nw;
    h = nh;
    pixels.assign(static_cast<size_t>(w) * h, 0);
    st = State();
    stack.clear();
    path.clear();
    dirty = true;
  }
};

Rgba paint_at(const Paint& p, double x, double y, const Matrix& ctm) {
  if (!p.gradient) return p.color;
  const GradientData& g = *p.gradient;
  Matrix inv;
  if (!ctm.invert(&inv)) return Rgba{0, 0, 0, 0};
  const Pt q = inv.apply(x, y);
  double t;
  if (!g.radial) {
    const double dx = g.x1 - g.x0, dy = g.y1 - g.y0, len2 = dx * dx + dy * dy;
    t = len2 == 0 ? 0 : ((q.x - g.x0) * dx + (q.y - g.y0) * dy) / len2;
  } else {
    // the circle through (x,y) between the two circles: solve for the largest t >= 0 giving r(t) >= 0
    const double cdx = g.x1 - g.x0, cdy = g.y1 - g.y0, dr = g.r1 - g.r0;
    const double px = q.x - g.x0, py = q.y - g.y0;
    const double A = cdx * cdx + cdy * cdy - dr * dr, B = px * cdx + py * cdy + g.r0 * dr, C = px * px + py * py - g.r0 * g.r0;
    if (std::fabs(A) < 1e-9) t = B == 0 ? 0 : C / (2 * B);
    else {
      const double disc = B * B - A * C;
      if (disc < 0) return Rgba{0, 0, 0, 0};
      const double s = std::sqrt(disc);
      double t1 = (B + s) / A, t2 = (B - s) / A;
      if (t1 < t2) std::swap(t1, t2);
      t = (g.r0 + t1 * dr >= 0) ? t1 : t2;
    }
  }
  if (g.stops.empty()) return Rgba{0, 0, 0, 0};
  t = std::clamp(t, 0.0, 1.0);
  if (t <= g.stops.front().first) return g.stops.front().second;
  for (size_t i = 1; i < g.stops.size(); ++i) {
    if (t <= g.stops[i].first) {
      const auto& a = g.stops[i - 1];
      const auto& b = g.stops[i];
      const double u = b.first == a.first ? 1 : (t - a.first) / (b.first - a.first);
      return Rgba{a.second.r + (b.second.r - a.second.r) * u, a.second.g + (b.second.g - a.second.g) * u, a.second.b + (b.second.b - a.second.b) * u,
                  a.second.a + (b.second.a - a.second.a) * u};
    }
  }
  return g.stops.back().second;
}

void blend_pixel(Canvas& c, int x, int y, const Rgba& src, double coverage) {
  if (x < 0 || y < 0 || x >= c.w || y >= c.h) return;
  if (!c.st.clip.empty()) coverage *= c.st.clip[static_cast<size_t>(y) * static_cast<size_t>(c.w) + static_cast<size_t>(x)] / 255.0;
  if (coverage <= 0) return;
  uint32_t& px = c.pixels[static_cast<size_t>(y) * static_cast<size_t>(c.w) + static_cast<size_t>(x)];
  const double da = ((px >> 24) & 0xFF) / 255.0, dr = (px >> 16) & 0xFF, dg = (px >> 8) & 0xFF, db = px & 0xFF;
  double oa, orr, og, ob;
  if (c.st.composite == "copy") {
    // the source replaces the destination where it covers, even when the source is transparent
    const double sa = src.a * c.st.global_alpha;
    oa = sa * coverage + da * (1 - coverage);
    if (oa <= 0) { px = 0; return; }
    orr = (src.r * sa * coverage + dr * da * (1 - coverage)) / oa;
    og = (src.g * sa * coverage + dg * da * (1 - coverage)) / oa;
    ob = (src.b * sa * coverage + db * da * (1 - coverage)) / oa;
  } else {
    const double a = src.a * c.st.global_alpha * coverage;
    if (a <= 0) return;
    if (c.st.composite == "destination-out") {
      oa = da * (1 - a);
      orr = dr; og = dg; ob = db;
    } else {
      oa = a + da * (1 - a);
      if (oa <= 0) { px = 0; return; }
      orr = (src.r * a + dr * da * (1 - a)) / oa;
      og = (src.g * a + dg * da * (1 - a)) / oa;
      ob = (src.b * a + db * da * (1 - a)) / oa;
    }
  }
  auto q = [](double v) { return static_cast<uint32_t>(std::clamp(std::lround(v), 0L, 255L)); };
  px = (q(oa * 255) << 24) | (q(orr) << 16) | (q(og) << 8) | q(ob);
}

// Scanline polygon fill with 4 sub-scanlines per row and exact horizontal span coverage.
struct Edge {
  double x0, y0, x1, y1;
  int dir;
};

void fill_polygons(Canvas& c, const std::vector<std::vector<Pt>>& polys, bool evenodd, const Paint& paint) {
  std::vector<Edge> edges;
  double miny = 1e18, maxy = -1e18;
  for (const auto& poly : polys) {
    const size_t n = poly.size();
    if (n < 2) continue;
    for (size_t i = 0; i < n; ++i) {
      const Pt& a = poly[i];
      const Pt& b = poly[(i + 1) % n];
      if (a.y == b.y) continue;
      Edge e;
      if (a.y < b.y) e = {a.x, a.y, b.x, b.y, 1};
      else e = {b.x, b.y, a.x, a.y, -1};
      edges.push_back(e);
      miny = std::min(miny, e.y0);
      maxy = std::max(maxy, e.y1);
    }
  }
  if (edges.empty()) return;
  const int y_start = std::max(0, static_cast<int>(std::floor(miny)));
  const int y_end = std::min(c.h - 1, static_cast<int>(std::ceil(maxy)));
  constexpr int kSub = 4;
  std::vector<double> cov(static_cast<size_t>(c.w) + 2);
  std::vector<std::pair<double, int>> xs;
  for (int y = y_start; y <= y_end; ++y) {
    std::fill(cov.begin(), cov.end(), 0.0);
    bool any = false;
    for (int s = 0; s < kSub; ++s) {
      const double sy = y + (s + 0.5) / kSub;
      xs.clear();
      for (const Edge& e : edges)
        if (sy >= e.y0 && sy < e.y1) xs.emplace_back(e.x0 + (e.x1 - e.x0) * (sy - e.y0) / (e.y1 - e.y0), e.dir);
      if (xs.empty()) continue;
      std::sort(xs.begin(), xs.end());
      int wind = 0;
      for (size_t i = 0; i + 1 < xs.size(); ++i) {
        wind += xs[i].second;
        const bool inside = evenodd ? ((i + 1) % 2 == 1) : wind != 0;
        if (!inside) continue;
        double x0 = std::max(0.0, xs[i].first), x1 = std::min(static_cast<double>(c.w), xs[i + 1].first);
        if (x1 <= x0) continue;
        any = true;
        const int p0 = static_cast<int>(std::floor(x0)), p1 = static_cast<int>(std::floor(x1));
        if (p0 == p1) cov[static_cast<size_t>(p0)] += (x1 - x0) / kSub;
        else {
          cov[static_cast<size_t>(p0)] += (p0 + 1 - x0) / kSub;
          for (int px = p0 + 1; px < p1; ++px) cov[static_cast<size_t>(px)] += 1.0 / kSub;
          if (p1 < c.w) cov[static_cast<size_t>(p1)] += (x1 - p1) / kSub;
        }
      }
    }
    if (!any) continue;
    for (int x = 0; x < c.w; ++x) {
      const double cv = cov[static_cast<size_t>(x)];
      if (cv <= 0) continue;
      Rgba col = paint_at(paint, x + 0.5, y + 0.5, c.st.ctm);
      blend_pixel(c, x, y, col, std::min(1.0, cv));
    }
  }
  c.dirty = true;
}

// ---- path building (points are kept in device space) ----

void add_point(Canvas& c, double ux, double uy, bool move) {
  Pt p = c.st.ctm.apply(ux, uy);
  if (move || c.path.empty() || c.path.back().closed) {
    Sub s;
    s.pts.push_back(p);
    c.path.push_back(std::move(s));
  } else {
    c.path.back().pts.push_back(p);
  }
}

bool ensure_subpath(Canvas& c, double ux, double uy) {
  if (c.path.empty() || c.path.back().closed) {
    add_point(c, ux, uy, true);
    return false;
  }
  return true;
}

Pt current_point(const Canvas& c) { return c.path.empty() || c.path.back().pts.empty() ? Pt{0, 0} : c.path.back().pts.back(); }

void bezier_points(Canvas& c, Pt p0, Pt p1, Pt p2, Pt p3) {
  const double len = std::hypot(p1.x - p0.x, p1.y - p0.y) + std::hypot(p2.x - p1.x, p2.y - p1.y) + std::hypot(p3.x - p2.x, p3.y - p2.y);
  const int n = std::clamp(static_cast<int>(len / 2), 8, 200);
  for (int i = 1; i <= n; ++i) {
    const double t = static_cast<double>(i) / n, u = 1 - t;
    const double x = u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * p3.x;
    const double y = u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t * t * t * p3.y;
    c.path.back().pts.push_back({x, y});
  }
}

void arc_path(Canvas& c, double cx, double cy, double rx, double ry, double rot, double a0, double a1, bool ccw) {
  const double two_pi = 6.283185307179586;
  double da = a1 - a0;
  if (!ccw) {
    if (da >= two_pi) da = two_pi;
    else { da = std::fmod(da, two_pi); if (da < 0) da += two_pi; }
  } else {
    if (da <= -two_pi) da = -two_pi;
    else { da = std::fmod(da, two_pi); if (da > 0) da -= two_pi; }
  }
  const double scale = c.st.ctm.scale_factor();
  const int steps = std::clamp(static_cast<int>(std::fabs(da) * std::max(rx, ry) * scale / 2.5), 12, 720);
  for (int i = 0; i <= steps; ++i) {
    const double ang = a0 + da * i / steps;
    const double ex = rx * std::cos(ang), ey = ry * std::sin(ang);
    const double ux = cx + ex * std::cos(rot) - ey * std::sin(rot), uy = cy + ex * std::sin(rot) + ey * std::cos(rot);
    if (i == 0) {
      if (ensure_subpath(c, ux, uy)) add_point(c, ux, uy, false);
    } else {
      add_point(c, ux, uy, false);
    }
  }
}

std::vector<std::vector<Pt>> all_polygons(const Canvas& c) {
  std::vector<std::vector<Pt>> polys;
  for (const Sub& s : c.path)
    if (s.pts.size() >= 2 || (s.pts.size() == 1 && s.closed)) polys.push_back(s.pts);
  return polys;
}

// ---- stroking ----

void circle_poly(std::vector<std::vector<Pt>>& out, Pt c, double r) {
  std::vector<Pt> poly;
  const int n = std::clamp(static_cast<int>(r * 3), 10, 64);
  for (int i = 0; i < n; ++i) {
    const double a = 6.283185307179586 * i / n;
    poly.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)});
  }
  out.push_back(std::move(poly));
}

// Apply the dash pattern to a polyline, producing open sub-polylines.
std::vector<std::vector<Pt>> dash_polyline(const std::vector<Pt>& pts, bool closed, const std::vector<double>& dash, double offset) {
  std::vector<std::vector<Pt>> out;
  if (dash.empty()) { out.push_back(pts); if (closed && !pts.empty()) out.back().push_back(pts.front()); return out; }
  std::vector<Pt> line = pts;
  if (closed && !line.empty()) line.push_back(line.front());
  size_t di = 0;
  double remain = dash[0], phase = std::fmod(offset, [&] { double t = 0; for (double d : dash) t += d; return t > 0 ? t : 1.0; }());
  if (phase < 0) phase += 1;
  while (phase > 0) {
    if (phase >= remain) { phase -= remain; di = (di + 1) % dash.size(); remain = dash[di]; }
    else { remain -= phase; phase = 0; }
  }
  bool on = di % 2 == 0;
  std::vector<Pt> cur;
  if (on && !line.empty()) cur.push_back(line[0]);
  for (size_t i = 1; i < line.size(); ++i) {
    Pt a = line[i - 1];
    const Pt b = line[i];
    double seg = std::hypot(b.x - a.x, b.y - a.y);
    while (seg > 0) {
      const double step = std::min(remain, seg);
      const double t = step / seg;
      Pt p{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
      if (on) cur.push_back(p);
      seg -= step;
      remain -= step;
      a = p;
      if (remain <= 1e-9) {
        if (on && cur.size() >= 2) out.push_back(cur);
        cur.clear();
        di = (di + 1) % dash.size();
        remain = std::max(dash[di], 1e-6);
        on = !on;
        if (on) cur.push_back(a);
      }
      if (step <= 0) break;
    }
  }
  if (on && cur.size() >= 2) out.push_back(cur);
  return out;
}

void stroke_path(Canvas& c) {
  const double lw = std::max(0.0, c.st.line_width) * c.st.ctm.scale_factor();
  if (lw <= 0) return;
  const double hw = lw / 2;
  std::vector<std::vector<Pt>> polys;
  const bool round_join = c.st.line_join == "round";
  const bool bevel_join = c.st.line_join == "bevel";
  const std::string& cap = c.st.line_cap;
  std::vector<double> dash;
  for (double d : c.st.dash) dash.push_back(d * c.st.ctm.scale_factor());
  for (const Sub& s : c.path) {
    if (s.pts.empty()) continue;
    std::vector<std::vector<Pt>> runs = dash_polyline(s.pts, s.closed, dash, c.st.dash_offset * c.st.ctm.scale_factor());
    for (const auto& run : runs) {
      // drop repeated points
      std::vector<Pt> pts;
      for (const Pt& p : run)
        if (pts.empty() || std::hypot(p.x - pts.back().x, p.y - pts.back().y) > 1e-9) pts.push_back(p);
      if (pts.size() == 1) {   // a dot
        if (cap == "round") circle_poly(polys, pts[0], hw);
        else if (cap == "square") polys.push_back({{pts[0].x - hw, pts[0].y - hw}, {pts[0].x + hw, pts[0].y - hw}, {pts[0].x + hw, pts[0].y + hw}, {pts[0].x - hw, pts[0].y + hw}});
        continue;
      }
      const bool is_closed_run = s.closed && dash.empty() && pts.size() > 2 && std::hypot(pts.front().x - pts.back().x, pts.front().y - pts.back().y) < 1e-9;
      if (is_closed_run) pts.pop_back();
      const size_t n = pts.size();
      const size_t segs = is_closed_run ? n : n - 1;
      for (size_t i = 0; i < segs; ++i) {
        Pt a = pts[i], b = pts[(i + 1) % n];
        const double dx = b.x - a.x, dy = b.y - a.y, len = std::hypot(dx, dy);
        if (len < 1e-12) continue;
        const double ux = dx / len, uy = dy / len, nx = -uy * hw, ny = ux * hw;
        Pt a2 = a, b2 = b;
        const bool first_open = !is_closed_run && i == 0, last_open = !is_closed_run && i == segs - 1;
        if (cap == "square" && first_open) { a2.x -= ux * hw; a2.y -= uy * hw; }
        if (cap == "square" && last_open) { b2.x += ux * hw; b2.y += uy * hw; }
        polys.push_back({{a2.x + nx, a2.y + ny}, {b2.x + nx, b2.y + ny}, {b2.x - nx, b2.y - ny}, {a2.x - nx, a2.y - ny}});
      }
      if (cap == "round" && !is_closed_run) {
        circle_poly(polys, pts.front(), hw);
        circle_poly(polys, pts.back(), hw);
      }
      // joins
      const size_t j_begin = is_closed_run ? 0 : 1, j_end = is_closed_run ? n : n - 1;
      for (size_t i = j_begin; i < j_end; ++i) {
        const Pt prev = pts[(i + n - 1) % n], at = pts[i], next = pts[(i + 1) % n];
        if (round_join) { circle_poly(polys, at, hw); continue; }
        const double d1x = at.x - prev.x, d1y = at.y - prev.y, d2x = next.x - at.x, d2y = next.y - at.y;
        const double l1 = std::hypot(d1x, d1y), l2 = std::hypot(d2x, d2y);
        if (l1 < 1e-12 || l2 < 1e-12) continue;
        const double u1x = d1x / l1, u1y = d1y / l1, u2x = d2x / l2, u2y = d2y / l2;
        const double cross = u1x * u2y - u1y * u2x;
        if (std::fabs(cross) < 1e-9) continue;
        const double side = cross > 0 ? -1 : 1;   // the outer side of the turn
        const Pt p1{at.x + side * (-u1y) * hw * -1, at.y + side * u1x * hw * -1};
        const Pt p2{at.x + side * (-u2y) * hw * -1, at.y + side * u2x * hw * -1};
        std::vector<Pt> wedge{at, p1, p2};
        if (!bevel_join) {
          // miter tip, if within the limit
          const double dot = u1x * u2x + u1y * u2y;
          const double miter = 1 / std::sqrt(std::max(1e-9, (1 + dot) / 2));
          if (miter <= c.st.miter_limit) {
            const double mx = p1.x + p2.x - 2 * at.x, my = p1.y + p2.y - 2 * at.y, ml = std::hypot(mx, my);
            if (ml > 1e-9) wedge = {at, p1, {at.x + mx / ml * hw * miter, at.y + my / ml * hw * miter}, p2};
          }
        }
        polys.push_back(wedge);
      }
    }
  }
  fill_polygons(c, polys, false, c.st.stroke);
}

// ---- text ----

double font_px(const std::string& font) {
  // the first number followed by px / pt / em / rem
  size_t i = 0;
  while (i < font.size()) {
    if (std::isdigit(static_cast<unsigned char>(font[i])) || (font[i] == '.' && i + 1 < font.size() && std::isdigit(static_cast<unsigned char>(font[i + 1])))) {
      char* end = nullptr;
      double v = std::strtod(font.c_str() + i, &end);
      std::string unit(end, std::min<size_t>(3, font.size() - static_cast<size_t>(end - font.c_str())));
      if (unit.compare(0, 2, "px") == 0) return v;
      if (unit.compare(0, 2, "pt") == 0) return v * 4.0 / 3.0;
      if (unit.compare(0, 3, "rem") == 0 || unit.compare(0, 2, "em") == 0) return v * 16;
      i = static_cast<size_t>(end - font.c_str());
      continue;
    }
    ++i;
  }
  return 10;
}

double text_width(const std::string& text, double px) {
  size_t chars = 0;
  for (unsigned char ch : text) if ((ch & 0xC0) != 0x80) ++chars;
  return static_cast<double>(chars) * 6.0 * (px / 8.0);
}

void draw_text(Canvas& c, const std::string& text, double ux, double uy, bool stroke) {
  const double px = font_px(c.st.font), cell = px / 8.0;
  const double width = text_width(text, px);
  double x0 = ux;
  if (c.st.text_align == "center") x0 -= width / 2;
  else if (c.st.text_align == "right" || c.st.text_align == "end") x0 -= width;
  double top = uy - 7 * cell;   // alphabetic baseline: the 7th row sits on it
  if (c.st.text_baseline == "top" || c.st.text_baseline == "hanging") top = uy;
  else if (c.st.text_baseline == "middle") top = uy - 3.5 * cell;
  else if (c.st.text_baseline == "bottom" || c.st.text_baseline == "ideographic") top = uy - 8 * cell;
  std::vector<std::vector<Pt>> polys;
  double pen = x0;
  for (size_t i = 0; i < text.size();) {
    unsigned char ch = static_cast<unsigned char>(text[i]);
    size_t len = 1;
    if (ch >= 0xF0) len = 4; else if (ch >= 0xE0) len = 3; else if (ch >= 0xC0) len = 2;
    i += len;
    const int idx = ch >= 32 && ch < 127 ? ch - 32 : (len > 1 ? 31 : -1);   // non-ASCII draws a "?"
    if (idx >= 0 && idx < 95) {
      for (int col = 0; col < 5; ++col) {
        const uint8_t bits = kFont[idx][col];
        for (int row = 0; row < 8; ++row) {
          if (!(bits & (1 << row))) continue;
          const double gx = pen + col * cell, gy = top + row * cell;
          const Pt p0 = c.st.ctm.apply(gx, gy), p1 = c.st.ctm.apply(gx + cell, gy), p2 = c.st.ctm.apply(gx + cell, gy + cell), p3 = c.st.ctm.apply(gx, gy + cell);
          polys.push_back({p0, p1, p2, p3});
        }
      }
    }
    pen += 6 * cell;
  }
  if (stroke) {
    // an outline of the glyph cells: a thin ring around every lit cell group is not worth its cost; stroke text is drawn as fill in the stroke colour
    fill_polygons(c, polys, false, c.st.stroke);
  } else {
    fill_polygons(c, polys, false, c.st.fill);
  }
}

// ---------------------------------------------------------------------------------------------------
// objects
// ---------------------------------------------------------------------------------------------------

struct ContextObject final : HostObject {
  ContextObject() : HostObject(HostKind::CanvasCtx) {}
  std::shared_ptr<Canvas> canvas;
  NodeObject* element = nullptr;
  void trace(Tracer& t) override { Object::trace(t); t.mark(element); }
};

struct ImageObject final : HostObject {
  ImageObject() : HostObject(HostKind::Image) {}
  web::Bitmap bitmap;
  std::string src;
};

}  // namespace

struct Page::CanvasHost {
  Object* ctx_proto = nullptr;
  Object* gradient_proto = nullptr;
  Object* imagedata_proto = nullptr;
  Object* image_proto = nullptr;
  std::map<Node*, std::shared_ptr<Canvas>> canvases;   // per <canvas> element
  std::map<Node*, ContextObject*> contexts;
};

namespace {

ContextObject* as_context(const Value& v) {
  if (!v.is_object() || v.as_object()->cls != js::Class::Host) return nullptr;
  Object* o = v.as_object();
  return static_cast<HostObject*>(o)->host_kind == HostKind::CanvasCtx ? static_cast<ContextObject*>(o) : nullptr;
}

ContextObject* this_ctx(Page& P, CallArgs& a) {
  ContextObject* c = as_context(a.this_value);
  if (!c) P.ctx.throw_type_error("Illegal invocation");
  return c;
}

GradientObject* as_gradient(const Value& v) {
  if (!v.is_object() || v.as_object()->cls != js::Class::Host) return nullptr;
  return static_cast<HostObject*>(v.as_object())->host_kind == HostKind::Gradient ? static_cast<GradientObject*>(v.as_object()) : nullptr;
}

void sync_dims(Canvas& c) {
  if (!c.owner) return;
  const std::string wa = c.owner->attr("width"), ha = c.owner->attr("height");
  const int w = wa.empty() ? 300 : std::atoi(wa.c_str()), h = ha.empty() ? 150 : std::atoi(ha.c_str());
  c.resize(w, h);
}

bool set_style(Page& P, Paint* paint, std::string* text, const Value& v) {
  if (GradientObject* g = as_gradient(v)) {
    paint->gradient = g->data;
    return true;
  }
  const std::string s = P.text(v);
  Rgba col;
  if (!parse_css_color(s, &col)) return true;   // an unparsable value is ignored, as a browser does
  paint->color = col;
  paint->gradient.reset();
  *text = s;
  return true;
}

double arg_num(Page& P, CallArgs& a, int i) { return P.number(a[i]); }

bool finite_args(Page& P, CallArgs& a, int n) {
  for (int i = 0; i < n; ++i)
    if (!std::isfinite(arg_num(P, a, i))) return false;
  return true;
}

// Decode image bytes into a bitmap.
bool decode_bytes(const std::string& bytes, web::Bitmap* out) {
  auto r = web::decode_image(aios::ByteSpan(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
  if (!r.ok()) return false;
  *out = r.value();
  return true;
}

web::Bitmap* image_pixels(Page& P, const Value& src, web::Bitmap* scratch) {
  if (!src.is_object()) return nullptr;
  Object* o = src.as_object();
  if (o->cls != js::Class::Host) return nullptr;
  HostObject* h = static_cast<HostObject*>(o);
  if (h->host_kind == HostKind::Image) return &static_cast<ImageObject*>(o)->bitmap;
  if (h->host_kind == HostKind::Node) {
    NodeObject* n = static_cast<NodeObject*>(o);
    if (!n->dom) return nullptr;
    if (n->dom->tag == "canvas") {
      auto it = P.canvas_host->canvases.find(n->dom);
      if (it == P.canvas_host->canvases.end()) return nullptr;
      scratch->w = it->second->w;
      scratch->h = it->second->h;
      scratch->pixels = it->second->pixels;
      return scratch;
    }
    if (n->dom->tag == "img" && P.fetch) {
      const std::string url = resolve(P, n->dom->attr("src"));
      auto got = P.fetch(url, {});
      if (got.ok() && decode_bytes(std::string(got->body.begin(), got->body.end()), scratch)) return scratch;
    }
  }
  return nullptr;
}

void draw_image(Page& P, Canvas& c, const web::Bitmap& src, double sx, double sy, double sw, double sh, double dx, double dy, double dw, double dh) {
  if (src.empty() || dw == 0 || dh == 0 || sw == 0 || sh == 0) return;
  Matrix inv;
  // map device pixels back to the source rectangle through the CTM and the destination rectangle
  Matrix dest = c.st.ctm * Matrix{dw / sw, 0, 0, dh / sh, dx - sx * dw / sw, dy - sy * dh / sh};
  if (!dest.invert(&inv)) return;
  const Pt q0 = c.st.ctm.apply(dx, dy), q1 = c.st.ctm.apply(dx + dw, dy), q2 = c.st.ctm.apply(dx + dw, dy + dh), q3 = c.st.ctm.apply(dx, dy + dh);
  const int x0 = std::max(0, static_cast<int>(std::floor(std::min({q0.x, q1.x, q2.x, q3.x})))), x1 = std::min(c.w - 1, static_cast<int>(std::ceil(std::max({q0.x, q1.x, q2.x, q3.x}))));
  const int y0 = std::max(0, static_cast<int>(std::floor(std::min({q0.y, q1.y, q2.y, q3.y})))), y1 = std::min(c.h - 1, static_cast<int>(std::ceil(std::max({q0.y, q1.y, q2.y, q3.y}))));
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) {
      const Pt s = inv.apply(x + 0.5, y + 0.5);
      if (s.x < sx || s.y < sy || s.x >= sx + sw || s.y >= sy + sh) continue;
      const int ix = std::clamp(static_cast<int>(std::floor(s.x)), 0, src.w - 1), iy = std::clamp(static_cast<int>(std::floor(s.y)), 0, src.h - 1);
      const uint32_t px = src.pixels[static_cast<size_t>(iy) * static_cast<size_t>(src.w) + static_cast<size_t>(ix)];
      Rgba col{static_cast<double>((px >> 16) & 0xFF), static_cast<double>((px >> 8) & 0xFF), static_cast<double>(px & 0xFF), ((px >> 24) & 0xFF) / 255.0};
      blend_pixel(c, x, y, col, 1.0);
    }
  c.dirty = true;
  (void)P;
}

Value make_image_data(Page& P, int w, int h, const std::vector<uint8_t>* rgba) {
  Object* o = P.ctx.new_object(P.canvas_host->imagedata_proto);
  Context& c = P.ctx;
  Value ctor;
  std::vector<uint8_t> data(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
  if (rgba) data = *rgba;
  Object* clamped = nullptr;
  {
    Value u8c, args[1] = {Value::Obj(c.new_array_buffer(data.data(), data.size()))};
    if (c.get(Value::Obj(c.global()), c.atom_ascii("Uint8ClampedArray"), &u8c)) {
      Value r;
      if (c.construct(u8c, args, 1, u8c, &r)) clamped = r.as_object();
      else c.clear_exception();
    }
  }
  c.define_data(o, c.atom_ascii("width"), Value::Number(w), js::kEnumerable);
  c.define_data(o, c.atom_ascii("height"), Value::Number(h), js::kEnumerable);
  c.define_data(o, c.atom_ascii("data"), clamped ? Value::Obj(clamped) : Value::Undefined(), js::kEnumerable);
  c.define_data(o, c.atom_ascii("colorSpace"), Value::String(c.atom_ascii("srgb")), js::kEnumerable);
  return Value::Obj(o);
}

void install_context_methods(Page& P) {
  Page* pp = &P;
  Object* cp = P.canvas_host->ctx_proto;
  auto M = [pp, cp](const char* name, int len, std::function<Value(ContextObject*, Canvas&, CallArgs&)> fn) {
    def_method(*pp, cp, name, len, [pp, fn](Context&, CallArgs& a) -> Value {
      ContextObject* co = this_ctx(*pp, a);
      if (!co) return Value::Exception();
      sync_dims(*co->canvas);
      return fn(co, *co->canvas, a);
    });
  };
  auto num = [pp](CallArgs& a, int i) { return pp->number(a[i]); };

  M("save", 0, [](ContextObject*, Canvas& c, CallArgs&) { c.stack.push_back(c.st); return Value::Undefined(); });
  M("restore", 0, [](ContextObject*, Canvas& c, CallArgs&) {
    if (!c.stack.empty()) { c.st = c.stack.back(); c.stack.pop_back(); }
    return Value::Undefined();
  });
  M("reset", 0, [](ContextObject*, Canvas& c, CallArgs&) {
    std::fill(c.pixels.begin(), c.pixels.end(), 0u);
    c.st = State();
    c.stack.clear();
    c.path.clear();
    c.dirty = true;
    return Value::Undefined();
  });
  M("scale", 2, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    if (finite_args(*pp, a, 2)) c.st.ctm = c.st.ctm * Matrix{num(a, 0), 0, 0, num(a, 1), 0, 0};
    return Value::Undefined();
  });
  M("translate", 2, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    if (finite_args(*pp, a, 2)) c.st.ctm = c.st.ctm * Matrix{1, 0, 0, 1, num(a, 0), num(a, 1)};
    return Value::Undefined();
  });
  M("rotate", 1, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    if (finite_args(*pp, a, 1)) {
      const double t = num(a, 0), cs = std::cos(t), sn = std::sin(t);
      c.st.ctm = c.st.ctm * Matrix{cs, sn, -sn, cs, 0, 0};
    }
    return Value::Undefined();
  });
  M("transform", 6, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    if (finite_args(*pp, a, 6)) c.st.ctm = c.st.ctm * Matrix{num(a, 0), num(a, 1), num(a, 2), num(a, 3), num(a, 4), num(a, 5)};
    return Value::Undefined();
  });
  M("setTransform", 6, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    if (a.argc >= 6 && finite_args(*pp, a, 6)) c.st.ctm = Matrix{num(a, 0), num(a, 1), num(a, 2), num(a, 3), num(a, 4), num(a, 5)};
    else if (a.argc == 0) c.st.ctm = Matrix();
    return Value::Undefined();
  });
  M("resetTransform", 0, [](ContextObject*, Canvas& c, CallArgs&) { c.st.ctm = Matrix(); return Value::Undefined(); });
  M("beginPath", 0, [](ContextObject*, Canvas& c, CallArgs&) { c.path.clear(); return Value::Undefined(); });
  M("closePath", 0, [](ContextObject*, Canvas& c, CallArgs&) {
    if (!c.path.empty() && !c.path.back().closed) {
      Sub& s = c.path.back();
      s.closed = true;
    }
    return Value::Undefined();
  });
  M("moveTo", 2, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    if (finite_args(*pp, a, 2)) add_point(c, num(a, 0), num(a, 1), true);
    return Value::Undefined();
  });
  M("lineTo", 2, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    if (!finite_args(*pp, a, 2)) return Value::Undefined();
    if (ensure_subpath(c, num(a, 0), num(a, 1))) add_point(c, num(a, 0), num(a, 1), false);
    return Value::Undefined();
  });
  M("quadraticCurveTo", 4, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    if (!finite_args(*pp, a, 4)) return Value::Undefined();
    ensure_subpath(c, num(a, 0), num(a, 1));
    const Pt p0 = current_point(c), p1 = c.st.ctm.apply(num(a, 0), num(a, 1)), p2 = c.st.ctm.apply(num(a, 2), num(a, 3));
    // raise to a cubic
    bezier_points(c, p0, {p0.x + 2.0 / 3 * (p1.x - p0.x), p0.y + 2.0 / 3 * (p1.y - p0.y)}, {p2.x + 2.0 / 3 * (p1.x - p2.x), p2.y + 2.0 / 3 * (p1.y - p2.y)}, p2);
    return Value::Undefined();
  });
  M("bezierCurveTo", 6, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    if (!finite_args(*pp, a, 6)) return Value::Undefined();
    ensure_subpath(c, num(a, 0), num(a, 1));
    bezier_points(c, current_point(c), c.st.ctm.apply(num(a, 0), num(a, 1)), c.st.ctm.apply(num(a, 2), num(a, 3)), c.st.ctm.apply(num(a, 4), num(a, 5)));
    return Value::Undefined();
  });
  M("arc", 5, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    if (!finite_args(*pp, a, 5)) return Value::Undefined();
    if (num(a, 2) < 0) { pp->ctx.throw_range_error("The radius provided (" + std::to_string(num(a, 2)) + ") is negative."); return Value::Exception(); }
    arc_path(c, num(a, 0), num(a, 1), num(a, 2), num(a, 2), 0, num(a, 3), num(a, 4), Context::to_boolean(a[5]));
    return Value::Undefined();
  });
  M("ellipse", 7, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    if (!finite_args(*pp, a, 7)) return Value::Undefined();
    arc_path(c, num(a, 0), num(a, 1), num(a, 2), num(a, 3), num(a, 4), num(a, 5), num(a, 6), Context::to_boolean(a[7]));
    return Value::Undefined();
  });
  M("arcTo", 5, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    if (!finite_args(*pp, a, 5)) return Value::Undefined();
    Matrix inv;
    if (!c.st.ctm.invert(&inv)) return Value::Undefined();
    ensure_subpath(c, num(a, 0), num(a, 1));
    const Pt p0 = inv.apply(current_point(c).x, current_point(c).y), p1{num(a, 0), num(a, 1)}, p2{num(a, 2), num(a, 3)};
    const double r = num(a, 4);
    const double v1x = p0.x - p1.x, v1y = p0.y - p1.y, v2x = p2.x - p1.x, v2y = p2.y - p1.y;
    const double l1 = std::hypot(v1x, v1y), l2 = std::hypot(v2x, v2y);
    const double cross = v1x * v2y - v1y * v2x;
    if (l1 == 0 || l2 == 0 || std::fabs(cross) < 1e-9 || r == 0) { add_point(c, p1.x, p1.y, false); return Value::Undefined(); }
    const double ang = std::acos(std::clamp((v1x * v2x + v1y * v2y) / (l1 * l2), -1.0, 1.0));
    const double tan_len = r / std::tan(ang / 2);
    const Pt t1{p1.x + v1x / l1 * tan_len, p1.y + v1y / l1 * tan_len}, t2{p1.x + v2x / l2 * tan_len, p1.y + v2y / l2 * tan_len};
    const double sign = cross > 0 ? 1 : -1;
    const Pt center{t1.x + sign * (-(v1y / l1)) * r * -1 * -1, t1.y + sign * (v1x / l1) * r * -1 * -1};
    const double a0 = std::atan2(t1.y - center.y, t1.x - center.x), a1 = std::atan2(t2.y - center.y, t2.x - center.x);
    add_point(c, t1.x, t1.y, false);
    arc_path(c, center.x, center.y, r, r, 0, a0, a1, sign < 0);
    return Value::Undefined();
  });
  M("rect", 4, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    if (!finite_args(*pp, a, 4)) return Value::Undefined();
    const double x = num(a, 0), y = num(a, 1), w = num(a, 2), h = num(a, 3);
    add_point(c, x, y, true);
    add_point(c, x + w, y, false);
    add_point(c, x + w, y + h, false);
    add_point(c, x, y + h, false);
    c.path.back().closed = true;
    return Value::Undefined();
  });
  M("roundRect", 5, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    if (!finite_args(*pp, a, 4)) return Value::Undefined();
    double x = num(a, 0), y = num(a, 1), w = num(a, 2), h = num(a, 3);
    double r = 0;
    if (a[4].is_number()) r = a[4].num;
    else if (a[4].is_object()) { Value v; pp->ctx.get_index(a[4], 0, &v); r = pp->number(v); }
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    r = std::clamp(r, 0.0, std::min(w, h) / 2);
    add_point(c, x + r, y, true);
    arc_path(c, x + w - r, y + r, r, r, 0, -1.5707963267948966, 0, false);
    arc_path(c, x + w - r, y + h - r, r, r, 0, 0, 1.5707963267948966, false);
    arc_path(c, x + r, y + h - r, r, r, 0, 1.5707963267948966, 3.141592653589793, false);
    arc_path(c, x + r, y + r, r, r, 0, 3.141592653589793, 4.71238898038469, false);
    c.path.back().closed = true;
    return Value::Undefined();
  });
  M("fill", 0, [pp](ContextObject*, Canvas& c, CallArgs& a) {
    const bool eo = a[0].is_string() ? pp->text(a[0]) == "evenodd" : (a[1].is_string() && pp->text(a[1]) == "evenodd");
    fill_polygons(c, all_polygons(c), eo, c.st.fill);
    return Value::Undefined();
  });
  M("stroke", 0, [](ContextObject*, Canvas& c, CallArgs&) { stroke_path(c); return Value::Undefined(); });
  M("clip", 0, [pp](ContextObject*, Canvas& c, CallArgs& a) {
    const bool eo = a[0].is_string() ? pp->text(a[0]) == "evenodd" : (a[1].is_string() && pp->text(a[1]) == "evenodd");
    // coverage of the current path becomes the new mask (intersected with any existing one)
    Canvas scratch;
    scratch.w = c.w;
    scratch.h = c.h;
    scratch.pixels.assign(static_cast<size_t>(c.w) * static_cast<size_t>(c.h), 0);
    scratch.st.fill.color = Rgba{255, 255, 255, 1};
    fill_polygons(scratch, all_polygons(c), eo, scratch.st.fill);
    std::vector<uint8_t> mask(scratch.pixels.size());
    for (size_t i = 0; i < mask.size(); ++i) {
      uint32_t m = (scratch.pixels[i] >> 24) & 0xFF;
      if (!c.st.clip.empty()) m = m * c.st.clip[i] / 255;
      mask[i] = static_cast<uint8_t>(m);
    }
    c.st.clip = std::move(mask);
    return Value::Undefined();
  });
  M("fillRect", 4, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    if (!finite_args(*pp, a, 4)) return Value::Undefined();
    const double x = num(a, 0), y = num(a, 1), w = num(a, 2), h = num(a, 3);
    std::vector<Pt> poly{c.st.ctm.apply(x, y), c.st.ctm.apply(x + w, y), c.st.ctm.apply(x + w, y + h), c.st.ctm.apply(x, y + h)};
    fill_polygons(c, {poly}, false, c.st.fill);
    return Value::Undefined();
  });
  M("strokeRect", 4, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    if (!finite_args(*pp, a, 4)) return Value::Undefined();
    std::vector<Sub> saved = c.path;
    c.path.clear();
    const double x = num(a, 0), y = num(a, 1), w = num(a, 2), h = num(a, 3);
    add_point(c, x, y, true);
    add_point(c, x + w, y, false);
    add_point(c, x + w, y + h, false);
    add_point(c, x, y + h, false);
    c.path.back().closed = true;
    stroke_path(c);
    c.path = saved;
    return Value::Undefined();
  });
  M("clearRect", 4, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    if (!finite_args(*pp, a, 4)) return Value::Undefined();
    const double x = num(a, 0), y = num(a, 1), w = num(a, 2), h = num(a, 3);
    std::vector<Pt> poly{c.st.ctm.apply(x, y), c.st.ctm.apply(x + w, y), c.st.ctm.apply(x + w, y + h), c.st.ctm.apply(x, y + h)};
    const std::string saved_op = c.st.composite;
    const double saved_alpha = c.st.global_alpha;
    c.st.composite = "copy";
    c.st.global_alpha = 1;
    Paint clear;
    clear.color = Rgba{0, 0, 0, 0};
    fill_polygons(c, {poly}, false, clear);
    c.st.composite = saved_op;
    c.st.global_alpha = saved_alpha;
    return Value::Undefined();
  });
  M("fillText", 3, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    if (finite_args(*pp, a, 0) && std::isfinite(num(a, 1)) && std::isfinite(num(a, 2))) draw_text(c, pp->text(a[0]), num(a, 1), num(a, 2), false);
    return Value::Undefined();
  });
  M("strokeText", 3, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    if (std::isfinite(num(a, 1)) && std::isfinite(num(a, 2))) draw_text(c, pp->text(a[0]), num(a, 1), num(a, 2), true);
    return Value::Undefined();
  });
  M("measureText", 1, [pp](ContextObject*, Canvas& c, CallArgs& a) {
    Object* r = pp->ctx.new_object();
    const double px = font_px(c.st.font);
    pp->ctx.define_data(r, pp->ctx.atom_ascii("width"), Value::Number(text_width(pp->text(a[0]), px)));
    pp->ctx.define_data(r, pp->ctx.atom_ascii("actualBoundingBoxLeft"), Value::Number(0));
    pp->ctx.define_data(r, pp->ctx.atom_ascii("actualBoundingBoxRight"), Value::Number(text_width(pp->text(a[0]), px)));
    pp->ctx.define_data(r, pp->ctx.atom_ascii("actualBoundingBoxAscent"), Value::Number(px * 7 / 8));
    pp->ctx.define_data(r, pp->ctx.atom_ascii("actualBoundingBoxDescent"), Value::Number(px / 8));
    pp->ctx.define_data(r, pp->ctx.atom_ascii("fontBoundingBoxAscent"), Value::Number(px * 7 / 8));
    pp->ctx.define_data(r, pp->ctx.atom_ascii("fontBoundingBoxDescent"), Value::Number(px / 8));
    return Value::Obj(r);
  });
  M("drawImage", 3, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    web::Bitmap scratch;
    web::Bitmap* src = image_pixels(*pp, a[0], &scratch);
    if (!src) {
      pp->ctx.throw_type_error("Failed to execute 'drawImage' on 'CanvasRenderingContext2D': The provided value is not of type '(CSSImageValue or HTMLImageElement or SVGImageElement or HTMLVideoElement or HTMLCanvasElement or ImageBitmap or OffscreenCanvas or VideoFrame)'.");
      return Value::Exception();
    }
    if (a.argc >= 9) draw_image(*pp, c, *src, num(a, 1), num(a, 2), num(a, 3), num(a, 4), num(a, 5), num(a, 6), num(a, 7), num(a, 8));
    else if (a.argc >= 5) draw_image(*pp, c, *src, 0, 0, src->w, src->h, num(a, 1), num(a, 2), num(a, 3), num(a, 4));
    else draw_image(*pp, c, *src, 0, 0, src->w, src->h, num(a, 1), num(a, 2), src->w, src->h);
    return Value::Undefined();
  });
  M("getImageData", 4, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    int sx = static_cast<int>(num(a, 0)), sy = static_cast<int>(num(a, 1)), sw = static_cast<int>(num(a, 2)), sh = static_cast<int>(num(a, 3));
    if (sw == 0 || sh == 0) { pp->ctx.throw_error(js::ErrorKind::Error, "The source width or height is 0"); return Value::Exception(); }
    if (sw < 0) { sx += sw; sw = -sw; }
    if (sh < 0) { sy += sh; sh = -sh; }
    if (static_cast<int64_t>(sw) * sh > 64 * 1024 * 1024) { pp->ctx.throw_range_error("Array buffer allocation failed"); return Value::Exception(); }
    std::vector<uint8_t> out(static_cast<size_t>(sw) * static_cast<size_t>(sh) * 4, 0);
    for (int y = 0; y < sh; ++y)
      for (int x = 0; x < sw; ++x) {
        const int cx = sx + x, cy = sy + y;
        if (cx < 0 || cy < 0 || cx >= c.w || cy >= c.h) continue;
        const uint32_t px = c.pixels[static_cast<size_t>(cy) * static_cast<size_t>(c.w) + static_cast<size_t>(cx)];
        const size_t o = (static_cast<size_t>(y) * static_cast<size_t>(sw) + static_cast<size_t>(x)) * 4;
        out[o] = static_cast<uint8_t>(px >> 16); out[o + 1] = static_cast<uint8_t>(px >> 8); out[o + 2] = static_cast<uint8_t>(px); out[o + 3] = static_cast<uint8_t>(px >> 24);
      }
    return make_image_data(*pp, sw, sh, &out);
  });
  M("putImageData", 3, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    if (!a[0].is_object()) { pp->ctx.throw_type_error("parameter 1 is not of type 'ImageData'."); return Value::Exception(); }
    Value wv, hv, dv;
    pp->ctx.get(a[0], pp->ctx.atom_ascii("width"), &wv);
    pp->ctx.get(a[0], pp->ctx.atom_ascii("height"), &hv);
    pp->ctx.get(a[0], pp->ctx.atom_ascii("data"), &dv);
    std::string bytes;
    if (!pp->ctx.bytes_of(dv, &bytes)) { pp->ctx.throw_type_error("parameter 1 is not of type 'ImageData'."); return Value::Exception(); }
    const int w = static_cast<int>(pp->number(wv)), h = static_cast<int>(pp->number(hv));
    const int dx = static_cast<int>(num(a, 1)), dy = static_cast<int>(num(a, 2));
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const int cx = dx + x, cy = dy + y;
        if (cx < 0 || cy < 0 || cx >= c.w || cy >= c.h) continue;
        const size_t o = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
        if (o + 3 >= bytes.size()) break;
        c.pixels[static_cast<size_t>(cy) * static_cast<size_t>(c.w) + static_cast<size_t>(cx)] =
            (static_cast<uint32_t>(static_cast<uint8_t>(bytes[o + 3])) << 24) | (static_cast<uint32_t>(static_cast<uint8_t>(bytes[o])) << 16) |
            (static_cast<uint32_t>(static_cast<uint8_t>(bytes[o + 1])) << 8) | static_cast<uint8_t>(bytes[o + 2]);
      }
    c.dirty = true;
    return Value::Undefined();
  });
  M("createImageData", 2, [pp](ContextObject*, Canvas&, CallArgs& a) {
    int w, h;
    if (a[0].is_object()) {
      Value wv, hv;
      pp->ctx.get(a[0], pp->ctx.atom_ascii("width"), &wv);
      pp->ctx.get(a[0], pp->ctx.atom_ascii("height"), &hv);
      w = static_cast<int>(pp->number(wv));
      h = static_cast<int>(pp->number(hv));
    } else {
      w = static_cast<int>(pp->number(a[0]));
      h = static_cast<int>(pp->number(a[1]));
    }
    if (w == 0 || h == 0) { pp->ctx.throw_error(js::ErrorKind::Error, "The source width or height is 0"); return Value::Exception(); }
    return make_image_data(*pp, std::abs(w), std::abs(h), nullptr);
  });
  M("createLinearGradient", 4, [pp, num](ContextObject*, Canvas&, CallArgs& a) {
    GradientObject* g = pp->ctx.heap.make<GradientObject>();
    g->proto = pp->canvas_host->gradient_proto;
    g->data->x0 = num(a, 0); g->data->y0 = num(a, 1); g->data->x1 = num(a, 2); g->data->y1 = num(a, 3);
    return Value::Obj(g);
  });
  M("createRadialGradient", 6, [pp, num](ContextObject*, Canvas&, CallArgs& a) {
    GradientObject* g = pp->ctx.heap.make<GradientObject>();
    g->proto = pp->canvas_host->gradient_proto;
    g->data->radial = true;
    g->data->x0 = num(a, 0); g->data->y0 = num(a, 1); g->data->r0 = num(a, 2); g->data->x1 = num(a, 3); g->data->y1 = num(a, 4); g->data->r1 = num(a, 5);
    return Value::Obj(g);
  });
  M("setLineDash", 1, [pp](ContextObject*, Canvas& c, CallArgs& a) {
    std::vector<Value> list;
    c.st.dash.clear();
    if (a[0].is_object() && pp->ctx.iterable_to_list(a[0], &list)) {
      for (const Value& v : list) { double d = pp->number(v); if (!std::isfinite(d) || d < 0) { c.st.dash.clear(); return Value::Undefined(); } c.st.dash.push_back(d); }
      if (c.st.dash.size() % 2) { auto copy = c.st.dash; c.st.dash.insert(c.st.dash.end(), copy.begin(), copy.end()); }
    }
    pp->ctx.clear_exception();
    return Value::Undefined();
  });
  M("getLineDash", 0, [pp](ContextObject*, Canvas& c, CallArgs&) {
    std::vector<Value> out;
    for (double d : c.st.dash) out.push_back(Value::Number(d));
    return make_array(*pp, out);
  });
  M("isPointInPath", 2, [pp, num](ContextObject*, Canvas& c, CallArgs& a) {
    // even-odd on the current path in device space
    const Pt p = c.st.ctm.apply(num(a, 0), num(a, 1));
    int wind = 0;
    for (const Sub& s : c.path) {
      const size_t n = s.pts.size();
      for (size_t i = 0; i < n; ++i) {
        const Pt& u = s.pts[i];
        const Pt& v = s.pts[(i + 1) % n];
        if ((u.y <= p.y) != (v.y <= p.y)) {
          const double x = u.x + (p.y - u.y) / (v.y - u.y) * (v.x - u.x);
          if (x > p.x) wind += v.y > u.y ? 1 : -1;
        }
      }
    }
    (void)pp;
    return Value::Bool(wind != 0);
  });
  M("getTransform", 0, [pp](ContextObject*, Canvas& c, CallArgs&) {
    Object* m = pp->ctx.new_object();
    const char* names[] = {"a", "b", "c", "d", "e", "f"};
    const double vals[] = {c.st.ctm.a, c.st.ctm.b, c.st.ctm.c, c.st.ctm.d, c.st.ctm.e, c.st.ctm.f};
    for (int i = 0; i < 6; ++i) pp->ctx.define_data(m, pp->ctx.atom_ascii(names[i]), Value::Number(vals[i]));
    return Value::Obj(m);
  });

  // state properties
  auto prop_str = [pp, cp](const char* name, std::function<std::string(Canvas&)> get, std::function<void(Canvas&, const std::string&)> set) {
    def_accessor(*pp, cp, name, [pp, get](Context&, CallArgs& a) -> Value {
      ContextObject* co = this_ctx(*pp, a);
      if (!co) return Value::Exception();
      return Value::String(pp->ctx.new_string_utf8(get(*co->canvas)));
    }, [pp, set](Context&, CallArgs& a) -> Value {
      ContextObject* co = this_ctx(*pp, a);
      if (!co) return Value::Exception();
      set(*co->canvas, pp->text(a[0]));
      return Value::Undefined();
    });
  };
  auto prop_num = [pp, cp](const char* name, std::function<double(Canvas&)> get, std::function<void(Canvas&, double)> set) {
    def_accessor(*pp, cp, name, [pp, get](Context&, CallArgs& a) -> Value {
      ContextObject* co = this_ctx(*pp, a);
      if (!co) return Value::Exception();
      return Value::Number(get(*co->canvas));
    }, [pp, set](Context&, CallArgs& a) -> Value {
      ContextObject* co = this_ctx(*pp, a);
      if (!co) return Value::Exception();
      double d = pp->number(a[0]);
      if (std::isfinite(d)) set(*co->canvas, d);
      return Value::Undefined();
    });
  };
  def_accessor(P, cp, "fillStyle", [pp](Context&, CallArgs& a) -> Value {
    ContextObject* co = this_ctx(*pp, a);
    if (!co) return Value::Exception();
    return Value::String(pp->ctx.new_string_utf8(co->canvas->st.fill_style_text));
  }, [pp](Context&, CallArgs& a) -> Value {
    ContextObject* co = this_ctx(*pp, a);
    if (!co) return Value::Exception();
    set_style(*pp, &co->canvas->st.fill, &co->canvas->st.fill_style_text, a[0]);
    return Value::Undefined();
  });
  def_accessor(P, cp, "strokeStyle", [pp](Context&, CallArgs& a) -> Value {
    ContextObject* co = this_ctx(*pp, a);
    if (!co) return Value::Exception();
    return Value::String(pp->ctx.new_string_utf8(co->canvas->st.stroke_style_text));
  }, [pp](Context&, CallArgs& a) -> Value {
    ContextObject* co = this_ctx(*pp, a);
    if (!co) return Value::Exception();
    set_style(*pp, &co->canvas->st.stroke, &co->canvas->st.stroke_style_text, a[0]);
    return Value::Undefined();
  });
  prop_num("lineWidth", [](Canvas& c) { return c.st.line_width; }, [](Canvas& c, double d) { if (d > 0) c.st.line_width = d; });
  prop_num("globalAlpha", [](Canvas& c) { return c.st.global_alpha; }, [](Canvas& c, double d) { if (d >= 0 && d <= 1) c.st.global_alpha = d; });
  prop_num("miterLimit", [](Canvas& c) { return c.st.miter_limit; }, [](Canvas& c, double d) { if (d > 0) c.st.miter_limit = d; });
  prop_num("lineDashOffset", [](Canvas& c) { return c.st.dash_offset; }, [](Canvas& c, double d) { c.st.dash_offset = d; });
  prop_str("lineCap", [](Canvas& c) { return c.st.line_cap; }, [](Canvas& c, const std::string& s) { if (s == "butt" || s == "round" || s == "square") c.st.line_cap = s; });
  prop_str("lineJoin", [](Canvas& c) { return c.st.line_join; }, [](Canvas& c, const std::string& s) { if (s == "miter" || s == "round" || s == "bevel") c.st.line_join = s; });
  prop_str("font", [](Canvas& c) { return c.st.font; }, [](Canvas& c, const std::string& s) { if (!s.empty()) c.st.font = s; });
  prop_str("textAlign", [](Canvas& c) { return c.st.text_align; }, [](Canvas& c, const std::string& s) { c.st.text_align = s; });
  prop_str("textBaseline", [](Canvas& c) { return c.st.text_baseline; }, [](Canvas& c, const std::string& s) { c.st.text_baseline = s; });
  prop_str("globalCompositeOperation", [](Canvas& c) { return c.st.composite; }, [](Canvas& c, const std::string& s) { c.st.composite = s; });
  prop_str("direction", [](Canvas&) { return std::string("ltr"); }, [](Canvas&, const std::string&) {});
  prop_str("imageSmoothingQuality", [](Canvas&) { return std::string("low"); }, [](Canvas&, const std::string&) {});
  def_accessor(P, cp, "imageSmoothingEnabled", [](Context&, CallArgs&) -> Value { return Value::Bool(true); }, [](Context&, CallArgs&) -> Value { return Value::Undefined(); });
  def_accessor(P, cp, "canvas", [pp](Context&, CallArgs& a) -> Value {
    ContextObject* co = this_ctx(*pp, a);
    if (!co) return Value::Exception();
    return co->element ? Value::Obj(co->element) : Value::Null();
  });
  for (const char* shadow : {"shadowBlur", "shadowOffsetX", "shadowOffsetY"})
    def_accessor(P, cp, shadow, [](Context&, CallArgs&) -> Value { return Value::Number(0); }, [](Context&, CallArgs&) -> Value { return Value::Undefined(); });
  def_accessor(P, cp, "shadowColor", [pp](Context&, CallArgs&) -> Value { return Value::String(pp->ctx.atom_ascii("rgba(0, 0, 0, 0)")); }, [](Context&, CallArgs&) -> Value { return Value::Undefined(); });
}

}  // namespace

// Copy dirty canvases into the document's inline images so layout and paint pick them up.
void sync_canvases(Page& P) {
  if (!P.doc || !P.canvas_host) return;
  for (auto& kv : P.canvas_host->canvases) {
    Canvas& c = *kv.second;
    if (!c.dirty) continue;
    c.dirty = false;
    char key[64];
    std::snprintf(key, sizeof key, "canvas://%p", static_cast<void*>(kv.first));
    web::Bitmap bm;
    bm.w = c.w;
    bm.h = c.h;
    bm.pixels = c.pixels;
    bool found = false;
    for (auto& img : P.doc->inline_images)
      if (img.first == key) { img.second = std::move(bm); found = true; break; }
    if (!found) P.doc->inline_images.push_back({key, std::move(bm)});
    P.write();
  }
}

void forget_canvas(Page& P, Node* n) {
  if (!P.canvas_host) return;
  P.canvas_host->canvases.erase(n);
  P.canvas_host->contexts.erase(n);
}

void trace_canvas(Page& P, js::Tracer& t) {
  if (!P.canvas_host) return;
  Page::CanvasHost& h = *P.canvas_host;
  t.mark(h.ctx_proto);
  t.mark(h.gradient_proto);
  t.mark(h.imagedata_proto);
  t.mark(h.image_proto);
  for (auto& kv : h.contexts) t.mark(kv.second);
}

namespace {

// ---------------------------------------------------------------------------------------------------
// Image, ImageData
// ---------------------------------------------------------------------------------------------------

ImageObject* as_image(const Value& v) {
  if (!v.is_object() || v.as_object()->cls != js::Class::Host) return nullptr;
  return static_cast<HostObject*>(v.as_object())->host_kind == HostKind::Image ? static_cast<ImageObject*>(v.as_object()) : nullptr;
}

void load_image(Page& P, ImageObject* img, const std::string& url) {
  img->src = url;
  img->bitmap = web::Bitmap();
  const std::string abs = resolve(P, url);
  std::string bytes;
  bool ok = false;
  if (abs.compare(0, 5, "data:") == 0) {
    const size_t comma = abs.find(',');
    if (comma != std::string::npos) {
      std::string payload = abs.substr(comma + 1);
      if (abs.substr(0, comma).find(";base64") != std::string::npos) {
        auto d = aios::base64_decode(payload);
        if (d.ok()) { bytes.assign(d->begin(), d->end()); ok = true; }
      }
    }
  } else if (P.fetch) {
    auto got = P.fetch(abs, {});
    if (got.ok()) { bytes.assign(got->body.begin(), got->body.end()); ok = true; }
  }
  ok = ok && decode_bytes(bytes, &img->bitmap);
  // load / error fire after the current task
  PendingTimer t;
  t.id = P.next_timer_id++;
  Page* pp = &P;
  Value self = Value::Obj(img);
  t.fn = Value::Obj(P.ctx.new_host_function("", [pp, self, ok](Context&, CallArgs&) -> Value {
    Object* ev = make_event(*pp, ok ? "load" : "error", "Event", false, false);
    dispatch_event_object(*pp, self.as_object(), self.as_object(), ev);
    return Value::Undefined();
  }, 0));
  P.add_timer(P.deferred, t, 2000);
}

void install_images(Page& P) {
  Page* pp = &P;
  P.canvas_host->imagedata_proto = make_proto(P, P.ctx.intr.object_proto, "ImageData");
  P.canvas_host->image_proto = make_proto(P, P.event_target_proto, "HTMLImageElement");
  def_ctor(P, "ImageData", 2, P.canvas_host->imagedata_proto, [pp](Context&, CallArgs& a) -> Value {
    if (!a.is_construct()) {
      pp->ctx.throw_type_error("Failed to construct 'ImageData': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
      return Value::Exception();
    }
    if (a[0].is_object()) {
      std::string bytes;
      if (!pp->ctx.bytes_of(a[0], &bytes)) { pp->ctx.throw_type_error("parameter 1 is not of type 'Uint8ClampedArray'."); return Value::Exception(); }
      const int w = static_cast<int>(pp->number(a[1]));
      const int h = a.argc > 2 ? static_cast<int>(pp->number(a[2])) : static_cast<int>(bytes.size() / 4 / std::max(1, w));
      if (w <= 0 || h <= 0 || bytes.size() != static_cast<size_t>(w) * static_cast<size_t>(h) * 4) {
        pp->ctx.throw_range_error("The input data length is not a multiple of (4 * width).");
        return Value::Exception();
      }
      std::vector<uint8_t> raw(bytes.begin(), bytes.end());
      return make_image_data(*pp, w, h, &raw);
    }
    const int w = static_cast<int>(pp->number(a[0])), h = static_cast<int>(pp->number(a[1]));
    if (w <= 0 || h <= 0) { pp->ctx.throw_range_error("The source width is zero or not a number."); return Value::Exception(); }
    return make_image_data(*pp, w, h, nullptr);
  });
  def_ctor(P, "Image", 0, P.canvas_host->image_proto, [pp](Context&, CallArgs& a) -> Value {
    ImageObject* img = pp->ctx.heap.make<ImageObject>();
    img->proto = pp->canvas_host->image_proto;
    if (a.argc > 0) pp->ctx.define_data(img, pp->ctx.atom_ascii("width"), a[0]);
    if (a.argc > 1) pp->ctx.define_data(img, pp->ctx.atom_ascii("height"), a[1]);
    return Value::Obj(img);
  });
  def_accessor(P, P.canvas_host->image_proto, "src", [pp](Context&, CallArgs& a) -> Value {
    if (!as_image(a.this_value)) { pp->ctx.throw_type_error("Illegal invocation"); return Value::Exception(); }
    return Value::String(pp->ctx.new_string_utf8(as_image(a.this_value)->src));
  }, [pp](Context&, CallArgs& a) -> Value {
    if (!as_image(a.this_value)) { pp->ctx.throw_type_error("Illegal invocation"); return Value::Exception(); }
    load_image(*pp, as_image(a.this_value), pp->text(a[0]));
    return Value::Undefined();
  });
  auto img_num = [pp](const char* name, bool height) {
    def_accessor(*pp, pp->canvas_host->image_proto, name, [pp, height](Context&, CallArgs& a) -> Value {
      if (!as_image(a.this_value)) { pp->ctx.throw_type_error("Illegal invocation"); return Value::Exception(); }
      ImageObject* img = as_image(a.this_value);
      return Value::Number(height ? img->bitmap.h : img->bitmap.w);
    });
  };
  img_num("naturalWidth", false);
  img_num("naturalHeight", true);
  def_accessor(P, P.canvas_host->image_proto, "complete", [](Context&, CallArgs&) -> Value { return Value::Bool(true); });
  def_method(P, P.canvas_host->image_proto, "decode", 0, [pp](Context&, CallArgs&) -> Value { return resolved_promise(*pp, Value::Undefined()); });
  def_method(P, P.ctx.global(), "createImageBitmap", 1, [pp](Context&, CallArgs& a) -> Value { return resolved_promise(*pp, a[0]); });
}

// ---------------------------------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------------------------------

void play_tone(double freq, double dur, float vol) {
  const uint32_t rate = 44100;
  size_t frames = static_cast<size_t>(rate * dur);
  if (frames > rate * 5) frames = rate * 5;
  std::vector<int16_t> samples(frames);
  for (size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / rate;
    const double env = 1.0 - static_cast<double>(i) / static_cast<double>(frames);
    samples[i] = static_cast<int16_t>(std::sin(6.283185307179586 * freq * t) * env * vol * 14000.0);
  }
  (void)hal::audio_play(samples.data(), frames, rate);
}

void install_audio(Page& P) {
  Page* pp = &P;
  Object* ap = make_proto(P, P.event_target_proto, "HTMLAudioElement");
  def_ctor(P, "Audio", 0, ap, [pp, ap](Context&, CallArgs& a) -> Value {
    Object* o = pp->ctx.new_object(ap);
    Context& c = pp->ctx;
    c.define_data(o, c.atom_ascii("src"), Value::String(c.new_string_utf8(a.argc ? pp->text(a[0]) : std::string())));
    c.define_data(o, c.atom_ascii("currentTime"), Value::Number(0));
    c.define_data(o, c.atom_ascii("duration"), Value::Number(NAN));
    c.define_data(o, c.atom_ascii("volume"), Value::Number(1));
    c.define_data(o, c.atom_ascii("paused"), Value::Bool(true));
    c.define_data(o, c.atom_ascii("ended"), Value::Bool(false));
    c.define_data(o, c.atom_ascii("muted"), Value::Bool(false));
    c.define_data(o, c.atom_ascii("loop"), Value::Bool(false));
    return Value::Obj(o);
  });
  def_method(P, ap, "play", 0, [pp](Context& c, CallArgs& a) -> Value {
    if (a.this_value.is_object()) c.define_data(a.this_value.as_object(), c.atom_ascii("paused"), Value::Bool(false));
    return resolved_promise(*pp, Value::Undefined());
  });
  def_method(P, ap, "pause", 0, [](Context& c, CallArgs& a) -> Value {
    if (a.this_value.is_object()) c.define_data(a.this_value.as_object(), c.atom_ascii("paused"), Value::Bool(true));
    return Value::Undefined();
  });
  def_method(P, ap, "load", 0, [](Context&, CallArgs&) -> Value { return Value::Undefined(); });
  def_method(P, ap, "canPlayType", 1, [](Context& c, CallArgs&) -> Value { return Value::String(c.atoms.empty); });

  // Web Audio: an oscillator through a gain node, played as a short tone
  Object* acp = make_proto(P, P.event_target_proto, "AudioContext");
  def_ctor(P, "AudioContext", 0, acp, [pp, acp](Context&, CallArgs&) -> Value {
    Object* o = pp->ctx.new_object(acp);
    Context& c = pp->ctx;
    c.define_data(o, c.atom_ascii("currentTime"), Value::Number(0));
    c.define_data(o, c.atom_ascii("sampleRate"), Value::Number(44100));
    c.define_data(o, c.atom_ascii("state"), Value::String(c.atom_ascii("running")));
    Object* dest = c.new_object();
    c.define_data(dest, c.atom_ascii("maxChannelCount"), Value::Number(2));
    c.define_data(o, c.atom_ascii("destination"), Value::Obj(dest));
    return Value::Obj(o);
  });
  Value ac_ctor;
  P.ctx.get(Value::Obj(P.ctx.global()), P.ctx.atom_ascii("AudioContext"), &ac_ctor);
  P.ctx.define_data(P.ctx.global(), P.ctx.atom_ascii("webkitAudioContext"), ac_ctor, js::kHiddenAttrs);
  def_method(P, acp, "createOscillator", 0, [pp](Context& c, CallArgs&) -> Value {
    Object* o = c.new_object();
    c.define_data(o, c.atom_ascii("type"), Value::String(c.atom_ascii("sine")));
    Object* fq = c.new_object();
    c.define_data(fq, c.atoms.value, Value::Number(440));
    def_method(*pp, fq, "setValueAtTime", 2, [](Context& cc, CallArgs& aa) -> Value {
      if (aa.this_value.is_object()) cc.define_data(aa.this_value.as_object(), cc.atoms.value, aa[0]);
      return aa.this_value;
    });
    c.define_data(o, c.atom_ascii("frequency"), Value::Obj(fq));
    def_method(*pp, o, "start", 0, [pp](Context& cc, CallArgs& aa) -> Value {
      double freq = 440;
      Value f, v;
      if (aa.this_value.is_object() && cc.get(aa.this_value, cc.atom_ascii("frequency"), &f) && f.is_object() && cc.get(f, cc.atoms.value, &v)) freq = pp->number(v);
      play_tone(freq > 20 ? freq : 440, 0.3, 1.0f);
      return Value::Undefined();
    });
    def_method(*pp, o, "stop", 0, [](Context&, CallArgs&) -> Value { return Value::Undefined(); });
    def_method(*pp, o, "connect", 1, [](Context&, CallArgs& aa) -> Value { return aa[0]; });
    def_method(*pp, o, "disconnect", 0, [](Context&, CallArgs&) -> Value { return Value::Undefined(); });
    return Value::Obj(o);
  });
  def_method(P, acp, "createGain", 0, [pp](Context& c, CallArgs&) -> Value {
    Object* g = c.new_object();
    Object* gain = c.new_object();
    c.define_data(gain, c.atoms.value, Value::Number(1));
    def_method(*pp, gain, "setValueAtTime", 2, [](Context& cc, CallArgs& aa) -> Value {
      if (aa.this_value.is_object()) cc.define_data(aa.this_value.as_object(), cc.atoms.value, aa[0]);
      return aa.this_value;
    });
    c.define_data(g, c.atom_ascii("gain"), Value::Obj(gain));
    def_method(*pp, g, "connect", 1, [](Context&, CallArgs& aa) -> Value { return aa[0]; });
    def_method(*pp, g, "disconnect", 0, [](Context&, CallArgs&) -> Value { return Value::Undefined(); });
    return Value::Obj(g);
  });
  for (const char* m : {"resume", "suspend", "close"})
    def_method(P, acp, m, 0, [pp](Context&, CallArgs&) -> Value { return resolved_promise(*pp, Value::Undefined()); });
}

}  // namespace

void install_canvas(Page& P) {
  Page* pp = &P;
  P.canvas_host = std::make_shared<Page::CanvasHost>();
  P.canvas_host->ctx_proto = make_proto(P, P.ctx.intr.object_proto, "CanvasRenderingContext2D");
  P.canvas_host->gradient_proto = make_proto(P, P.ctx.intr.object_proto, "CanvasGradient");
  def_ctor(P, "CanvasRenderingContext2D", 0, P.canvas_host->ctx_proto, [](Context& c, CallArgs&) -> Value {
    c.throw_type_error("Illegal constructor");
    return Value::Exception();
  });
  def_ctor(P, "CanvasGradient", 0, P.canvas_host->gradient_proto, [](Context& c, CallArgs&) -> Value {
    c.throw_type_error("Illegal constructor");
    return Value::Exception();
  });
  def_method(P, P.canvas_host->gradient_proto, "addColorStop", 2, [pp](Context&, CallArgs& a) -> Value {
    GradientObject* g = as_gradient(a.this_value);
    if (!g) { pp->ctx.throw_type_error("Illegal invocation"); return Value::Exception(); }
    const double off = pp->number(a[0]);
    Rgba col;
    if (!(off >= 0 && off <= 1)) { pp->ctx.throw_error(js::ErrorKind::Error, "The provided value is not between 0 and 1"); return Value::Exception(); }
    if (!parse_css_color(pp->text(a[1]), &col)) { pp->ctx.throw_syntax_error("The value provided ('" + pp->text(a[1]) + "') could not be parsed as a color."); return Value::Exception(); }
    g->data->stops.emplace_back(off, col);
    std::stable_sort(g->data->stops.begin(), g->data->stops.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    return Value::Undefined();
  });
  install_context_methods(P);
  install_images(P);
  install_audio(P);

  // HTMLCanvasElement.getContext / toDataURL / toBlob
  Object* canvas_proto = P.tag_protos["canvas"];
  def_method(P, canvas_proto, "getContext", 1, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* n = this_node(*pp, a);
    if (!n) return Value::Exception();
    if (pp->text(a[0]) != "2d" || !n->dom) return Value::Null();
    auto it = pp->canvas_host->contexts.find(n->dom);
    if (it != pp->canvas_host->contexts.end()) return Value::Obj(it->second);
    auto canvas = std::make_shared<Canvas>();
    canvas->owner = n->dom;
    sync_dims(*canvas);
    pp->canvas_host->canvases[n->dom] = canvas;
    ContextObject* co = pp->ctx.heap.make<ContextObject>();
    co->proto = pp->canvas_host->ctx_proto;
    co->canvas = canvas;
    co->element = n;
    pp->canvas_host->contexts[n->dom] = co;
    return Value::Obj(co);
  });
  def_method(P, canvas_proto, "toDataURL", 0, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* n = this_node(*pp, a);
    if (!n) return Value::Exception();
    auto it = n->dom ? pp->canvas_host->canvases.find(n->dom) : pp->canvas_host->canvases.end();
    if (it == pp->canvas_host->canvases.end() || it->second->pixels.empty()) {
      // a canvas nothing was drawn on: a blank picture of its size
      std::vector<uint32_t> blank(static_cast<size_t>(300) * 150, 0);
      ByteBuffer png = web::encode_png(blank.data(), 300, 150);
      return Value::String(pp->ctx.new_string_utf8("data:image/png;base64," + aios::base64_encode(ByteSpan(png.data(), png.size()))));
    }
    Canvas& c = *it->second;
    ByteBuffer png = web::encode_png(c.pixels.data(), c.w, c.h);
    return Value::String(pp->ctx.new_string_utf8("data:image/png;base64," + aios::base64_encode(ByteSpan(png.data(), png.size()))));
  });
  def_method(P, canvas_proto, "toBlob", 1, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* n = this_node(*pp, a);
    if (!n) return Value::Exception();
    if (!a[0].is_callable()) { pp->ctx.throw_type_error("The callback provided as parameter 1 is not a function."); return Value::Exception(); }
    auto it = n->dom ? pp->canvas_host->canvases.find(n->dom) : pp->canvas_host->canvases.end();
    std::string bytes;
    if (it != pp->canvas_host->canvases.end()) {
      Canvas& c = *it->second;
      ByteBuffer png = web::encode_png(c.pixels.data(), c.w, c.h);
      bytes.assign(png.begin(), png.end());
    }
    Value blob_ctor, blob, args[2];
    Object* part = pp->ctx.new_uint8_array(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    args[0] = Value::Obj(pp->ctx.new_array_from({Value::Obj(part)}));
    Object* opts = pp->ctx.new_object();
    pp->ctx.define_data(opts, pp->ctx.atom_ascii("type"), Value::String(pp->ctx.atom_ascii("image/png")));
    args[1] = Value::Obj(opts);
    if (!pp->ctx.get(Value::Obj(pp->ctx.global()), pp->ctx.atom_ascii("Blob"), &blob_ctor) || !pp->ctx.construct(blob_ctor, args, 2, blob_ctor, &blob)) return Value::Exception();
    PendingTimer t;
    t.id = pp->next_timer_id++;
    t.fn = a[0];
    t.args.push_back(blob);
    pp->add_timer(pp->deferred, t, 2000);
    return Value::Undefined();
  });
}

}  // namespace aios::userland::webjs
