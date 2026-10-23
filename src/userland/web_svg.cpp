// Inline SVG, rasterised.
//
// Every icon in a contemporary interface is an inline <svg> of stroked paths --
// the Assay console has forty-nine of them, all `viewBox="0 0 24 24"`,
// `fill="none"`, `stroke="currentColor"`. Without this they are nothing: the
// element has no text, so the inline collector found no words and the page drew
// an empty box where the icon goes. A sidebar of unlabelled grey squares is
// what that looks like.
//
// The approach is the one that fits in a kernel: flatten everything to
// polygons, rasterise at 3x into a coverage buffer, and box-filter down. No
// analytic antialiasing, no curve subdivision heuristics, no float-heavy
// scanline converter with an active edge table -- 3x3 supersampling of a
// 24-pixel icon is 5184 samples, which is nothing, and it gets the edges right
// because the edges are what the eye is looking at.
//
// What it draws: path (M m L l H h V v C c S s Q q T t A a Z z), rect (with rx),
// circle, ellipse, line, polyline, polygon, <g> for the attributes it carries
// down, <use> (of an element or a <symbol> with its own viewBox), nested <svg>,
// and `transform` on any of them. Paint is a colour (any CSS colour), or a
// linear or radial gradient by `url(#id)` with its stops, units, transform and
// href chain; `fill-opacity`, `stroke-opacity`, `opacity` and `style="..."`
// declarations apply. Fill is nonzero winding, or even-odd. Strokes are
// round-joined and round-capped, which is what every icon set of this shape
// asks for anyway. Not drawn: clip paths, masks, patterns, filters, markers,
// <text>, and a radial gradient's focal point.
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "aios/userland/web.hpp"

namespace aios::userland::web {
namespace {

constexpr int kSS = 3;                  // supersample factor per axis
constexpr int kMaxSide = 512;           // an icon, not a poster

struct Pt { float x, y; };

// One subpath, already flattened to line segments.
struct Poly {
  std::vector<Pt> pts;
  bool closed = false;
};

// A 2D affine map: (x, y) -> (a x + c y + e, b x + d y + f), the matrix SVG's `transform` writes.
struct Xf {
  float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  Pt map(Pt p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
  // This map applied after `in`: the outer frame of a child drawn with its own transform.
  Xf then(const Xf& in) const {
    Xf o;
    o.a = a * in.a + c * in.b;
    o.b = b * in.a + d * in.b;
    o.c = a * in.c + c * in.d;
    o.d = b * in.c + d * in.d;
    o.e = a * in.e + c * in.f + e;
    o.f = b * in.e + d * in.f + f;
    return o;
  }
  // How much a length grows under the map (the root of the area scale), for stroke widths and radii.
  float scale() const { return std::sqrt(std::fabs(a * d - b * c)); }
  bool invert(Xf& out) const {
    const float det = a * d - b * c;
    if (std::fabs(det) < 1e-12f) return false;
    out.a = d / det; out.b = -b / det; out.c = -c / det; out.d = a / det;
    out.e = -(out.a * e + out.c * f);
    out.f = -(out.b * e + out.d * f);
    return true;
  }
};

// What a fill or a stroke is painted with: nothing, a colour (with its own alpha), or a gradient element by id.
struct Paint {
  bool has = false;
  uint32_t color = 0xFF000000;
  std::string ref;                        // `url(#id)`: the gradient's id
};

struct Ctx {
  Paint fill;
  Paint stroke;
  float stroke_w = 1.0f;
  float fill_opacity = 1.0f, stroke_opacity = 1.0f, opacity = 1.0f;
  uint32_t current = 0xFF000000;          // `color`, what currentColor means
  Xf xf;                                  // user space -> canvas samples
};

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == ','; }

float to_f(const std::string& s, float dflt = 0.0f) {
  if (s.empty()) return dflt;
  return (float)std::atof(s.c_str());
}

std::string lowered_trim(const std::string& raw) {
  std::string v;
  for (char c : raw) if (c != ' ' && c != '\t' && c != '\n' && c != '\r') v += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  return v;
}

// `none`, `currentColor`, `url(#id)` (with an optional fallback colour after it), or any CSS colour. False when
// the value is none of those, which leaves the inherited paint in place, as an invalid presentation attribute does.
bool parse_paint(const std::string& raw, uint32_t current, Paint& out) {
  const std::string low = lowered_trim(raw);
  if (low.empty()) return false;
  if (low == "none" || low == "transparent") { out = Paint{}; return true; }
  if (low == "currentcolor" || low == "inherit") { out = Paint{}; out.has = true; out.color = current; return true; }
  if (low.compare(0, 4, "url(") == 0) {
    // The id keeps its case: ids are case-sensitive, so it is read from the raw text.
    const size_t hash = raw.find('#');
    const size_t close = raw.find(')', hash == std::string::npos ? 0 : hash);
    if (hash == std::string::npos || close == std::string::npos) return false;
    out = Paint{};
    out.has = true;
    out.ref = raw.substr(hash + 1, close - hash - 1);
    while (!out.ref.empty() && (out.ref.back() == '"' || out.ref.back() == '\'' || out.ref.back() == ' ')) out.ref.pop_back();
    // A fallback after the url, for when the reference does not resolve.
    uint32_t fb = 0;
    out.color = css_colour(raw.substr(close + 1), fb) ? fb : current;
    return true;
  }
  uint32_t c = 0;
  if (!css_colour(raw, c)) return false;
  out = Paint{};
  out.has = (c >> 24) != 0;
  out.color = c;
  return true;
}

// ------------------------------------------------------------ path parsing --

struct PathReader {
  const std::string& d;
  size_t i = 0;
  explicit PathReader(const std::string& s) : d(s) {}
  void skip() { while (i < d.size() && is_space(d[i])) ++i; }
  bool done() { skip(); return i >= d.size(); }
  bool next_num(float& out) {
    skip();
    size_t start = i;
    if (i < d.size() && (d[i] == '+' || d[i] == '-')) ++i;
    bool any = false;
    while (i < d.size() && d[i] >= '0' && d[i] <= '9') { ++i; any = true; }
    if (i < d.size() && d[i] == '.') {
      ++i;
      while (i < d.size() && d[i] >= '0' && d[i] <= '9') { ++i; any = true; }
    }
    if (any && i < d.size() && (d[i] == 'e' || d[i] == 'E')) {
      size_t save = i;
      ++i;
      if (i < d.size() && (d[i] == '+' || d[i] == '-')) ++i;
      bool ed = false;
      while (i < d.size() && d[i] >= '0' && d[i] <= '9') { ++i; ed = true; }
      if (!ed) i = save;
    }
    if (!any) { i = start; return false; }
    out = (float)std::atof(d.substr(start, i - start).c_str());
    return true;
  }
  // An arc flag is one digit, 0 or 1, and the next number may follow with no
  // separator: `a7 7 0 015.6 11` is flags 0,1 then x=5.6. Read as numbers the
  // flags swallow the coordinate, which blanks every Codex/Vector icon.
  bool next_flag(float& out) {
    skip();
    if (i < d.size() && (d[i] == '0' || d[i] == '1')) {
      out = d[i] == '1' ? 1.0f : 0.0f;
      ++i;
      return true;
    }
    return false;
  }
  bool next_cmd(char& out) {
    skip();
    if (i >= d.size()) return false;
    const char c = d[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) { out = c; ++i; return true; }
    return false;
  }
};

void flatten_cubic(std::vector<Pt>& into, Pt p0, Pt p1, Pt p2, Pt p3) {
  // Fixed subdivision. An icon is at most a couple of dozen device pixels
  // across; sixteen segments per curve is already finer than the sample grid.
  const int steps = 16;
  for (int s = 1; s <= steps; ++s) {
    const float t = (float)s / (float)steps, u = 1.0f - t;
    const float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, e = t * t * t;
    into.push_back({a * p0.x + b * p1.x + c * p2.x + e * p3.x,
                    a * p0.y + b * p1.y + c * p2.y + e * p3.y});
  }
}

void flatten_arc(std::vector<Pt>& into, Pt from, float rx, float ry, float rot_deg,
                 bool large, bool sweep, Pt to) {
  if (rx == 0 || ry == 0) { into.push_back(to); return; }
  rx = std::fabs(rx); ry = std::fabs(ry);
  const float rad = rot_deg * 3.14159265358979f / 180.0f;
  const float cosr = std::cos(rad), sinr = std::sin(rad);
  const float dx2 = (from.x - to.x) / 2.0f, dy2 = (from.y - to.y) / 2.0f;
  const float x1 = cosr * dx2 + sinr * dy2;
  const float y1 = -sinr * dx2 + cosr * dy2;
  float lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
  if (lam > 1.0f) { const float s = std::sqrt(lam); rx *= s; ry *= s; }
  float num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
  float den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
  if (den == 0) { into.push_back(to); return; }
  if (num < 0) num = 0;
  float co = std::sqrt(num / den);
  if (large == sweep) co = -co;
  const float cx1 = co * rx * y1 / ry;
  const float cy1 = -co * ry * x1 / rx;
  const float cx = cosr * cx1 - sinr * cy1 + (from.x + to.x) / 2.0f;
  const float cy = sinr * cx1 + cosr * cy1 + (from.y + to.y) / 2.0f;
  auto angle = [](float ux, float uy, float vx, float vy) {
    const float dot = ux * vx + uy * vy;
    const float len = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
    if (len == 0) return 0.0f;
    float c = dot / len;
    if (c < -1) c = -1;
    if (c > 1) c = 1;
    float a = std::acos(c);
    if (ux * vy - uy * vx < 0) a = -a;
    return a;
  };
  const float theta = angle(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
  float dtheta = angle((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
  const float two_pi = 6.283185307179586f;
  if (!sweep && dtheta > 0) dtheta -= two_pi;
  else if (sweep && dtheta < 0) dtheta += two_pi;
  const int steps = 24;
  for (int s = 1; s <= steps; ++s) {
    const float t = theta + dtheta * (float)s / (float)steps;
    const float ex = rx * std::cos(t), ey = ry * std::sin(t);
    into.push_back({cosr * ex - sinr * ey + cx, sinr * ex + cosr * ey + cy});
  }
}

std::vector<Poly> parse_path(const std::string& d) {
  std::vector<Poly> out;
  Poly cur;
  Pt pen{0, 0}, start{0, 0};
  Pt last_c{0, 0}, last_q{0, 0};
  char prev = 0;
  PathReader r(d);
  auto close_cur = [&](bool closed) {
    if (cur.pts.size() > 1) { cur.closed = closed; out.push_back(cur); }
    cur.pts.clear();
  };
  char cmd = 0;
  while (!r.done()) {
    char c;
    if (r.next_cmd(c)) cmd = c;
    else if (!cmd) break;                 // numbers with no command: nothing to do
    else if (cmd == 'M') cmd = 'L';       // repeated moveto pairs are linetos
    else if (cmd == 'm') cmd = 'l';
    const bool rel = cmd >= 'a' && cmd <= 'z';
    const char up = rel ? (char)(cmd - 'a' + 'A') : cmd;
    float a[7];
    switch (up) {
      case 'M': {
        if (!r.next_num(a[0]) || !r.next_num(a[1])) return out;
        close_cur(false);
        pen = rel ? Pt{pen.x + a[0], pen.y + a[1]} : Pt{a[0], a[1]};
        start = pen;
        cur.pts.push_back(pen);
        break;
      }
      case 'L': {
        if (!r.next_num(a[0]) || !r.next_num(a[1])) return out;
        pen = rel ? Pt{pen.x + a[0], pen.y + a[1]} : Pt{a[0], a[1]};
        cur.pts.push_back(pen);
        break;
      }
      case 'H': {
        if (!r.next_num(a[0])) return out;
        pen = rel ? Pt{pen.x + a[0], pen.y} : Pt{a[0], pen.y};
        cur.pts.push_back(pen);
        break;
      }
      case 'V': {
        if (!r.next_num(a[0])) return out;
        pen = rel ? Pt{pen.x, pen.y + a[0]} : Pt{pen.x, a[0]};
        cur.pts.push_back(pen);
        break;
      }
      case 'C': {
        for (int k = 0; k < 6; ++k) if (!r.next_num(a[k])) return out;
        const Pt p1 = rel ? Pt{pen.x + a[0], pen.y + a[1]} : Pt{a[0], a[1]};
        const Pt p2 = rel ? Pt{pen.x + a[2], pen.y + a[3]} : Pt{a[2], a[3]};
        const Pt p3 = rel ? Pt{pen.x + a[4], pen.y + a[5]} : Pt{a[4], a[5]};
        if (cur.pts.empty()) cur.pts.push_back(pen);
        flatten_cubic(cur.pts, pen, p1, p2, p3);
        last_c = p2;
        pen = p3;
        break;
      }
      case 'S': {
        for (int k = 0; k < 4; ++k) if (!r.next_num(a[k])) return out;
        const bool had = prev == 'C' || prev == 'c' || prev == 'S' || prev == 's';
        const Pt p1 = had ? Pt{2 * pen.x - last_c.x, 2 * pen.y - last_c.y} : pen;
        const Pt p2 = rel ? Pt{pen.x + a[0], pen.y + a[1]} : Pt{a[0], a[1]};
        const Pt p3 = rel ? Pt{pen.x + a[2], pen.y + a[3]} : Pt{a[2], a[3]};
        if (cur.pts.empty()) cur.pts.push_back(pen);
        flatten_cubic(cur.pts, pen, p1, p2, p3);
        last_c = p2;
        pen = p3;
        break;
      }
      case 'Q': {
        for (int k = 0; k < 4; ++k) if (!r.next_num(a[k])) return out;
        const Pt q = rel ? Pt{pen.x + a[0], pen.y + a[1]} : Pt{a[0], a[1]};
        const Pt p3 = rel ? Pt{pen.x + a[2], pen.y + a[3]} : Pt{a[2], a[3]};
        // A quadratic is a cubic with the control points a third of the way in.
        const Pt c1{pen.x + 2.0f / 3.0f * (q.x - pen.x), pen.y + 2.0f / 3.0f * (q.y - pen.y)};
        const Pt c2{p3.x + 2.0f / 3.0f * (q.x - p3.x), p3.y + 2.0f / 3.0f * (q.y - p3.y)};
        if (cur.pts.empty()) cur.pts.push_back(pen);
        flatten_cubic(cur.pts, pen, c1, c2, p3);
        last_q = q;
        pen = p3;
        break;
      }
      case 'T': {
        for (int k = 0; k < 2; ++k) if (!r.next_num(a[k])) return out;
        const bool had = prev == 'Q' || prev == 'q' || prev == 'T' || prev == 't';
        const Pt q = had ? Pt{2 * pen.x - last_q.x, 2 * pen.y - last_q.y} : pen;
        const Pt p3 = rel ? Pt{pen.x + a[0], pen.y + a[1]} : Pt{a[0], a[1]};
        const Pt c1{pen.x + 2.0f / 3.0f * (q.x - pen.x), pen.y + 2.0f / 3.0f * (q.y - pen.y)};
        const Pt c2{p3.x + 2.0f / 3.0f * (q.x - p3.x), p3.y + 2.0f / 3.0f * (q.y - p3.y)};
        if (cur.pts.empty()) cur.pts.push_back(pen);
        flatten_cubic(cur.pts, pen, c1, c2, p3);
        last_q = q;
        pen = p3;
        break;
      }
      case 'A': {
        for (int k = 0; k < 7; ++k)
          if (!((k == 3 || k == 4) ? r.next_flag(a[k]) : r.next_num(a[k]))) return out;
        const Pt to = rel ? Pt{pen.x + a[5], pen.y + a[6]} : Pt{a[5], a[6]};
        if (cur.pts.empty()) cur.pts.push_back(pen);
        flatten_arc(cur.pts, pen, a[0], a[1], a[2], a[3] != 0, a[4] != 0, to);
        pen = to;
        break;
      }
      case 'Z': {
        close_cur(true);
        pen = start;
        cur.pts.push_back(pen);
        break;
      }
      default:
        return out;                       // a command this does not read
    }
    prev = cmd;
  }
  close_cur(false);
  return out;
}

// ------------------------------------------------------------- rasterising --

// Samples at kSS x kSS per output pixel, each straight (not premultiplied) ARGB. A shape is first marked into
// `mask` -- which samples it covers -- and then painted once over what is there, so a translucent stroke whose
// segments overlap is still one even wash, and a gradient is evaluated once per covered sample.
struct Canvas {
  int w = 0, h = 0;
  std::vector<uint32_t> px;
  std::vector<uint8_t> mask;
  int mx0 = 0, my0 = 0, mx1 = 0, my1 = 0;   // where marks are, [x0, x1) x [y0, y1); empty when x0 >= x1
  void mark(int x, int y) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    mask[(size_t)y * w + x] = 1;
    if (mx0 >= mx1) { mx0 = x; mx1 = x + 1; my0 = y; my1 = y + 1; return; }
    if (x < mx0) mx0 = x;
    if (x >= mx1) mx1 = x + 1;
    if (y < my0) my0 = y;
    if (y >= my1) my1 = y + 1;
  }
};

// Source-over, straight alpha.
inline void blend(uint32_t& dst, uint32_t src) {
  const uint32_t sa = src >> 24;
  if (!sa) return;
  const uint32_t da = dst >> 24;
  if (sa == 255 || !da) { dst = src; return; }
  const float as = (float)sa / 255.0f, ad = (float)da / 255.0f;
  const float ao = as + ad * (1.0f - as);
  auto ch = [&](int shift) {
    const float cs = (float)((src >> shift) & 0xFF), cd = (float)((dst >> shift) & 0xFF);
    return (uint32_t)std::lround((cs * as + cd * ad * (1.0f - as)) / ao) & 0xFF;
  };
  dst = ((uint32_t)std::lround(ao * 255.0f) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// Scanline fill of a set of polygons into the mask: nonzero winding, or even-odd for `fill-rule="evenodd"`.
void fill_polys(Canvas& cv, const std::vector<Poly>& polys, bool even_odd = false) {
  float miny = 1e9f, maxy = -1e9f;
  for (const auto& p : polys)
    for (const auto& q : p.pts) { if (q.y < miny) miny = q.y; if (q.y > maxy) maxy = q.y; }
  if (maxy < miny) return;
  int y0 = (int)std::floor(miny), y1 = (int)std::ceil(maxy);
  if (y0 < 0) y0 = 0;
  if (y1 > cv.h) y1 = cv.h;

  struct Cross { float x; int dir; };
  std::vector<Cross> xs;
  for (int y = y0; y < y1; ++y) {
    const float sy = (float)y + 0.5f;
    xs.clear();
    for (const auto& p : polys) {
      const size_t n = p.pts.size();
      if (n < 2) continue;
      for (size_t k = 0; k < n; ++k) {
        const Pt& a = p.pts[k];
        const Pt& b = p.pts[(k + 1) % n];   // implicit close for filling
        if (a.y == b.y) continue;
        const float lo = a.y < b.y ? a.y : b.y;
        const float hi = a.y < b.y ? b.y : a.y;
        if (sy < lo || sy >= hi) continue;
        const float t = (sy - a.y) / (b.y - a.y);
        xs.push_back({a.x + t * (b.x - a.x), b.y > a.y ? 1 : -1});
      }
    }
    if (xs.size() < 2) continue;
    for (size_t i = 1; i < xs.size(); ++i) {
      Cross key = xs[i];
      size_t j = i;
      while (j > 0 && xs[j - 1].x > key.x) { xs[j] = xs[j - 1]; --j; }
      xs[j] = key;
    }
    int wind = 0;
    for (size_t i = 0; i + 1 < xs.size(); ++i) {
      wind += even_odd ? 1 : xs[i].dir;
      if (even_odd ? (wind & 1) == 0 : wind == 0) continue;
      int xa = (int)std::ceil(xs[i].x - 0.5f);
      int xb = (int)std::ceil(xs[i + 1].x - 0.5f);
      if (xa < 0) xa = 0;
      if (xb > cv.w) xb = cv.w;
      for (int x = xa; x < xb; ++x) cv.mark(x, y);
    }
  }
}

void fill_disc(Canvas& cv, float cx, float cy, float r) {
  if (r <= 0) return;
  const int x0 = (int)std::floor(cx - r), x1 = (int)std::ceil(cx + r);
  const int y0 = (int)std::floor(cy - r), y1 = (int)std::ceil(cy + r);
  const float r2 = r * r;
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) {
      const float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
      if (dx * dx + dy * dy <= r2) cv.mark(x, y);
    }
}

// A stroked polyline into the mask: each segment as a filled quad, each join and cap as a disc. Round joins and
// caps are what this icon shape asks for, and a disc is both of them.
void stroke_polys(Canvas& cv, const std::vector<Poly>& polys, float width) {
  const float r = width / 2.0f;
  if (r <= 0.05f) return;
  auto segment = [&](Pt a, Pt b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.0001f) { fill_disc(cv, a.x, a.y, r); return; }
    const float nx = -dy / len * r, ny = dx / len * r;
    Poly quad;
    quad.pts = {{a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny}};
    quad.closed = true;
    std::vector<Poly> one{quad};
    fill_polys(cv, one);
    fill_disc(cv, a.x, a.y, r);
  };
  for (const auto& p : polys) {
    const size_t n = p.pts.size();
    if (n == 1) { fill_disc(cv, p.pts[0].x, p.pts[0].y, r); continue; }
    for (size_t k = 0; k + 1 < n; ++k) segment(p.pts[k], p.pts[k + 1]);
    fill_disc(cv, p.pts[n - 1].x, p.pts[n - 1].y, r);
    if (p.closed && n > 2) segment(p.pts[n - 1], p.pts[0]);
  }
}

std::vector<Poly> shape_of(const Node& n) {
  std::vector<Poly> out;
  const std::string& t = n.tag;
  if (t == "path") return parse_path(n.attr("d"));
  if (t == "line") {
    Poly p;
    p.pts = {{to_f(n.attr("x1")), to_f(n.attr("y1"))}, {to_f(n.attr("x2")), to_f(n.attr("y2"))}};
    out.push_back(p);
    return out;
  }
  if (t == "polyline" || t == "polygon") {
    Poly p;
    const std::string& s = n.attr("points");
    size_t i = 0;
    while (i < s.size()) {
      while (i < s.size() && is_space(s[i])) ++i;
      size_t j = i;
      while (j < s.size() && !is_space(s[j])) ++j;
      if (j > i) {
        const float x = to_f(s.substr(i, j - i));
        i = j;
        while (i < s.size() && is_space(s[i])) ++i;
        j = i;
        while (j < s.size() && !is_space(s[j])) ++j;
        if (j > i) p.pts.push_back({x, to_f(s.substr(i, j - i))});
      }
      i = j;
    }
    p.closed = (t == "polygon");
    if (p.pts.size() > 1) out.push_back(p);
    return out;
  }
  if (t == "rect") {
    const float x = to_f(n.attr("x")), y = to_f(n.attr("y"));
    const float w = to_f(n.attr("width")), h = to_f(n.attr("height"));
    if (w <= 0 || h <= 0) return out;
    float rx = to_f(n.attr("rx"), 0), ry = to_f(n.attr("ry"), 0);
    if (rx <= 0) rx = ry;
    if (ry <= 0) ry = rx;
    if (rx > w / 2) rx = w / 2;
    if (ry > h / 2) ry = h / 2;
    Poly p;
    if (rx <= 0.01f) {
      p.pts = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    } else {
      const int steps = 8;
      auto corner = [&](float cx, float cy, float a0) {
        for (int s = 0; s <= steps; ++s) {
          const float a = a0 + (3.14159265f / 2.0f) * (float)s / (float)steps;
          p.pts.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a)});
        }
      };
      corner(x + w - rx, y + ry, -1.5707963f);
      corner(x + w - rx, y + h - ry, 0.0f);
      corner(x + rx, y + h - ry, 1.5707963f);
      corner(x + rx, y + ry, 3.14159265f);
    }
    p.closed = true;
    out.push_back(p);
    return out;
  }
  if (t == "circle" || t == "ellipse") {
    const float cx = to_f(n.attr("cx")), cy = to_f(n.attr("cy"));
    float rx, ry;
    if (t == "circle") { rx = ry = to_f(n.attr("r")); }
    else { rx = to_f(n.attr("rx")); ry = to_f(n.attr("ry")); }
    if (rx <= 0 || ry <= 0) return out;
    Poly p;
    const int steps = 40;
    for (int s = 0; s < steps; ++s) {
      const float a = 6.283185307f * (float)s / (float)steps;
      p.pts.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a)});
    }
    p.closed = true;
    out.push_back(p);
    return out;
  }
  return out;
}

// `transform="translate(..) scale(..) rotate(..) skewX(..) skewY(..) matrix(..)"`, applied left to right.
Xf parse_transform(const std::string& s) {
  Xf out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (is_space(s[i]))) ++i;
    size_t open = s.find('(', i);
    if (open == std::string::npos) break;
    std::string name = lowered_trim(s.substr(i, open - i));
    size_t close = s.find(')', open);
    if (close == std::string::npos) break;
    std::vector<float> v;
    const std::string args = s.substr(open + 1, close - open - 1);   // PathReader keeps a reference: not a temporary
    PathReader r(args);
    float num = 0;
    while (v.size() < 6 && r.next_num(num)) v.push_back(num);
    Xf t;
    const float deg = 3.14159265f / 180.0f;
    if (name == "translate" && !v.empty()) { t.e = v[0]; t.f = v.size() > 1 ? v[1] : 0; }
    else if (name == "scale" && !v.empty()) { t.a = v[0]; t.d = v.size() > 1 ? v[1] : v[0]; }
    else if (name == "rotate" && !v.empty()) {
      const float a = v[0] * deg, cs = std::cos(a), sn = std::sin(a);
      t.a = cs; t.b = sn; t.c = -sn; t.d = cs;
      if (v.size() >= 3) {   // about (cx, cy): translate there, rotate, translate back
        Xf to, back;
        to.e = v[1]; to.f = v[2];
        back.e = -v[1]; back.f = -v[2];
        t = to.then(t).then(back);
      }
    } else if (name == "skewx" && !v.empty()) { t.c = std::tan(v[0] * deg); }
    else if (name == "skewy" && !v.empty()) { t.b = std::tan(v[0] * deg); }
    else if (name == "matrix" && v.size() == 6) { t.a = v[0]; t.b = v[1]; t.c = v[2]; t.d = v[3]; t.e = v[4]; t.f = v[5]; }
    out = out.then(t);
    i = close + 1;
  }
  return out;
}

// The element with this id under `root`, or null.
const Node* by_id(const Node& root, const std::string& id, int depth = 0) {
  if (id.empty() || depth > 64) return nullptr;
  if (root.kind == Node::Kind::Element && root.attr_ref("id") == id) return &root;
  for (const auto& kid : root.children)
    if (kid)
      if (const Node* hit = by_id(*kid, id, depth + 1)) return hit;
  return nullptr;
}

// `href` or the older `xlink:href`, as `#id` -> `id`.
std::string href_id(const Node& n) {
  std::string h = n.attr("href");
  if (h.empty()) h = n.attr("xlink:href");
  if (h.size() > 1 && h[0] == '#') return h.substr(1);
  return {};
}

// `style="fill: #f00; stroke-width: 2"`: one property out of it, or empty.
std::string style_prop(const Node& n, const char* prop) {
  const std::string& st = n.attr_ref("style");
  if (st.empty()) return {};
  const size_t plen = std::strlen(prop);
  size_t i = 0;
  while (i < st.size()) {
    size_t semi = st.find(';', i);
    if (semi == std::string::npos) semi = st.size();
    const size_t colon = st.find(':', i);
    if (colon != std::string::npos && colon < semi) {
      std::string name = lowered_trim(st.substr(i, colon - i));
      if (name.size() == plen && name == prop) {
        std::string v = st.substr(colon + 1, semi - colon - 1);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
        return v;
      }
    }
    i = semi + 1;
  }
  return {};
}

// A presentation property: the `style` declaration wins over the attribute, as in a browser.
std::string prop_of(const Node& n, const char* prop) {
  std::string v = style_prop(n, prop);
  return v.empty() ? n.attr(prop) : v;
}

// A gradient, resolved through its `href` chain: the stops and each geometry attribute come from the first
// element in the chain that has them.
struct Gradient {
  bool radial = false;
  bool user_space = false;                // gradientUnits="userSpaceOnUse"; else objectBoundingBox
  float x1 = 0, y1 = 0, x2 = 1, y2 = 0;   // linear
  float cx = 0.5f, cy = 0.5f, r = 0.5f;   // radial
  Xf gxf;                                 // gradientTransform
  std::vector<std::pair<float, uint32_t>> stops;
};

// A coordinate: a percentage is a fraction (of the box, or of the viewport for user space), a number itself.
float coord(const std::string& v, float dflt, bool user_space, float span) {
  if (v.empty()) return dflt;
  const float n = to_f(v);
  if (v.back() == '%') return user_space ? n / 100.0f * span : n / 100.0f;
  return n;
}

bool load_gradient(const Node& root, const std::string& id, float vw, float vh, Gradient& g) {
  std::vector<const Node*> chain;
  for (const Node* n = by_id(root, id); n && chain.size() < 8; n = by_id(root, href_id(*n))) {
    if (n->tag != "lineargradient" && n->tag != "radialgradient") break;
    chain.push_back(n);
  }
  if (chain.empty()) return false;
  g.radial = chain[0]->tag == "radialgradient";
  auto first_attr = [&](const char* name) -> std::string {
    for (const Node* n : chain) if (n->has_attr(name)) return n->attr(name);
    return {};
  };
  g.user_space = lowered_trim(first_attr("gradientunits")) == "userspaceonuse";
  const float diag = std::sqrt((vw * vw + vh * vh) / 2.0f);
  if (g.radial) {
    g.cx = coord(first_attr("cx"), g.user_space ? vw / 2 : 0.5f, g.user_space, vw);
    g.cy = coord(first_attr("cy"), g.user_space ? vh / 2 : 0.5f, g.user_space, vh);
    g.r = coord(first_attr("r"), g.user_space ? diag / 2 : 0.5f, g.user_space, diag);
  } else {
    g.x1 = coord(first_attr("x1"), 0.0f, g.user_space, vw);
    g.y1 = coord(first_attr("y1"), 0.0f, g.user_space, vh);
    g.x2 = coord(first_attr("x2"), g.user_space ? vw : 1.0f, g.user_space, vw);
    g.y2 = coord(first_attr("y2"), 0.0f, g.user_space, vh);
  }
  g.gxf = parse_transform(first_attr("gradienttransform"));
  for (const Node* n : chain) {
    for (const auto& kid : n->children) {
      if (!kid || kid->kind != Node::Kind::Element || kid->tag != "stop") continue;
      std::string off = kid->attr("offset");
      float o = off.empty() ? 0.0f : to_f(off);
      if (!off.empty() && off.back() == '%') o /= 100.0f;
      o = o < 0 ? 0 : o > 1 ? 1 : o;
      if (!g.stops.empty() && o < g.stops.back().first) o = g.stops.back().first;   // offsets never go back
      uint32_t c = 0xFF000000;
      const std::string col = prop_of(*kid, "stop-color");
      if (!col.empty() && !css_colour(col, c)) c = 0xFF000000;
      const std::string op = prop_of(*kid, "stop-opacity");
      if (!op.empty()) {
        float a = to_f(op, 1.0f);
        a = a < 0 ? 0 : a > 1 ? 1 : a;
        c = ((uint32_t)std::lround((float)(c >> 24) * a) << 24) | (c & 0xFFFFFF);
      }
      g.stops.push_back({o, c});
    }
    if (!g.stops.empty()) break;
  }
  return !g.stops.empty();
}

uint32_t stop_colour(const Gradient& g, float t) {
  if (t <= g.stops.front().first) return g.stops.front().second;
  if (t >= g.stops.back().first) return g.stops.back().second;
  for (size_t k = 1; k < g.stops.size(); ++k) {
    if (t > g.stops[k].first) continue;
    const float a = g.stops[k - 1].first, b = g.stops[k].first;
    const float u = b > a ? (t - a) / (b - a) : 1.0f;
    const uint32_t p = g.stops[k - 1].second, q = g.stops[k].second;
    auto mix = [&](int sh) { return (uint32_t)std::lround(((p >> sh) & 0xFF) * (1 - u) + ((q >> sh) & 0xFF) * u) & 0xFF; };
    return (mix(24) << 24) | (mix(16) << 16) | (mix(8) << 8) | mix(0);
  }
  return g.stops.back().second;
}

struct Render {
  const Node* root = nullptr;
  float vw = 24, vh = 24;                 // the root viewBox, what user-space percentages are of
  int uses = 0;                           // <use> expansions so far: a self-referencing drawing stops here
};

// Paints the marked samples with `paint` at `alpha` (0..1) and clears the marks. `bbox` is the shape's box in
// its own user space, which an objectBoundingBox gradient is laid out in.
void composite(Canvas& cv, const Paint& paint, float alpha, const Ctx& ctx, const Render& rd,
               float bx0, float by0, float bx1, float by1) {
  if (cv.mx0 >= cv.mx1) return;
  Gradient g;
  bool grad = !paint.ref.empty() && load_gradient(*rd.root, paint.ref, rd.vw, rd.vh, g);
  Xf to_grad;   // canvas sample -> gradient space
  if (grad) {
    Xf box;      // objectBoundingBox: the unit square onto the shape's box
    if (!g.user_space) { box.a = bx1 - bx0; box.d = by1 - by0; box.e = bx0; box.f = by0; }
    if (!ctx.xf.then(box).then(g.gxf).invert(to_grad)) grad = false;
  }
  const uint32_t solid = paint.color;
  for (int y = cv.my0; y < cv.my1; ++y) {
    for (int x = cv.mx0; x < cv.mx1; ++x) {
      uint8_t& m = cv.mask[(size_t)y * cv.w + x];
      if (!m) continue;
      m = 0;
      uint32_t c = solid;
      if (grad) {
        const Pt p = to_grad.map({(float)x + 0.5f, (float)y + 0.5f});
        float t;
        if (g.radial) {
          const float dx = p.x - g.cx, dy = p.y - g.cy;
          t = g.r > 0 ? std::sqrt(dx * dx + dy * dy) / g.r : 1.0f;
        } else {
          const float ddx = g.x2 - g.x1, ddy = g.y2 - g.y1;
          const float len2 = ddx * ddx + ddy * ddy;
          t = len2 > 0 ? ((p.x - g.x1) * ddx + (p.y - g.y1) * ddy) / len2 : 0.0f;
        }
        c = stop_colour(g, t < 0 ? 0 : t > 1 ? 1 : t);   // spreadMethod pad
      }
      const uint32_t a = (uint32_t)std::lround((float)(c >> 24) * alpha);
      if (!a) continue;
      blend(cv.px[(size_t)y * cv.w + x], (a << 24) | (c & 0xFFFFFF));
    }
  }
  cv.mx0 = cv.mx1 = cv.my0 = cv.my1 = 0;
}

float opacity_of(const std::string& v, float dflt) {
  if (v.empty()) return dflt;
  float a = to_f(v, dflt);
  if (v.back() == '%') a /= 100.0f;
  return a < 0 ? 0 : a > 1 ? 1 : a;
}

// The viewBox `vb` fitted into a w x h viewport by `preserveAspectRatio` (default xMidYMid meet).
Xf fit_viewbox(float vx, float vy, float vw, float vh, float w, float h, const std::string& par_raw) {
  Xf t;
  if (vw <= 0 || vh <= 0) return t;
  const std::string par = lowered_trim(par_raw);
  float sx = w / vw, sy = h / vh;
  if (par.compare(0, 4, "none") != 0) {
    const bool slice = par.find("slice") != std::string::npos;
    const float s = slice ? std::max(sx, sy) : std::min(sx, sy);
    sx = sy = s;
  }
  float ax = 0.5f, ay = 0.5f;   // xMid YMid
  if (par.find("xmin") != std::string::npos) ax = 0;
  if (par.find("xmax") != std::string::npos) ax = 1;
  if (par.find("ymin") != std::string::npos) ay = 0;
  if (par.find("ymax") != std::string::npos) ay = 1;
  t.a = sx; t.d = sy;
  t.e = (w - vw * sx) * ax - vx * sx;
  t.f = (h - vh * sy) * ay - vy * sy;
  return t;
}

void draw_node(Canvas& cv, const Node& n, Ctx ctx, Render& rd, int depth, bool referenced = false);

void draw_children(Canvas& cv, const Node& n, const Ctx& ctx, Render& rd, int depth) {
  for (const auto& kid : n.children) if (kid) draw_node(cv, *kid, ctx, rd, depth + 1);
}

// `referenced`: drawn through a <use>, so a <symbol> (otherwise only a definition) is drawn.
void draw_node(Canvas& cv, const Node& n, Ctx ctx, Render& rd, int depth, bool referenced) {
  if (depth > 32) return;
  if (n.kind != Node::Kind::Element) return;
  const std::string& t = n.tag;
  // These hold DEFINITIONS -- a clip region, a gradient, a mask, a symbol to be
  // <use>d -- not pictures. Drawn as if they were, the full-frame `<path
  // fill="#fff">` inside a <clipPath> went over the whole drawing: Wikipedia's
  // wordmark came out solid white on a white page.
  if (t == "defs" || t == "clippath" || t == "mask" || (t == "symbol" && !referenced) ||
      t == "lineargradient" || t == "radialgradient" || t == "pattern" || t == "marker" || t == "style" ||
      t == "title" || t == "desc" || t == "metadata" || t == "filter" || t == "script" || t == "text")
    return;
  if (lowered_trim(prop_of(n, "display")) == "none") return;
  if (lowered_trim(prop_of(n, "visibility")) == "hidden") return;

  // Presentation properties carry down through <g> exactly as they do in a stylesheet, which is how an icon set
  // puts stroke="currentColor" once on the <svg> and never mentions it again.
  const std::string col = prop_of(n, "color");
  uint32_t cc = 0;
  if (!col.empty() && lowered_trim(col) != "currentcolor" && css_colour(col, cc)) ctx.current = cc;
  Paint p;
  const std::string fill = prop_of(n, "fill");
  if (!fill.empty() && parse_paint(fill, ctx.current, p)) ctx.fill = p;
  const std::string stroke = prop_of(n, "stroke");
  if (!stroke.empty() && parse_paint(stroke, ctx.current, p)) ctx.stroke = p;
  const std::string sw = prop_of(n, "stroke-width");
  if (!sw.empty()) {
    const float w = to_f(sw, ctx.stroke_w);
    if (w >= 0) ctx.stroke_w = w;
  }
  ctx.fill_opacity = opacity_of(prop_of(n, "fill-opacity"), ctx.fill_opacity);
  ctx.stroke_opacity = opacity_of(prop_of(n, "stroke-opacity"), ctx.stroke_opacity);
  ctx.opacity *= opacity_of(prop_of(n, "opacity"), 1.0f);   // not inherited: it multiplies down the tree
  if (n.has_attr("transform")) ctx.xf = ctx.xf.then(parse_transform(n.attr("transform")));

  if (t == "use") {
    const Node* target = rd.root ? by_id(*rd.root, href_id(n)) : nullptr;
    if (!target || ++rd.uses > 256) return;
    Xf at;
    at.e = to_f(n.attr("x"));
    at.f = to_f(n.attr("y"));
    ctx.xf = ctx.xf.then(at);
    if (target->tag == "symbol") {
      // A symbol is its own viewport: its viewBox fitted into the <use>'s width and height (100% by default).
      float vx = 0, vy = 0, vw = 0, vh = 0;
      const std::string vb = target->attr("viewbox");
      PathReader r(vb);
      if (!vb.empty() && r.next_num(vx) && r.next_num(vy) && r.next_num(vw) && r.next_num(vh) && vw > 0 && vh > 0) {
        const float w = to_f(n.attr("width"), rd.vw), h = to_f(n.attr("height"), rd.vh);
        ctx.xf = ctx.xf.then(fit_viewbox(vx, vy, vw, vh, w, h, target->attr("preserveaspectratio")));
      }
      draw_node(cv, *target, ctx, rd, depth + 1, true);
    } else {
      draw_node(cv, *target, ctx, rd, depth + 1, true);
    }
    return;
  }
  if (t == "svg" && &n != rd.root) {
    // A nested viewport: placed at x, y, its viewBox fitted into its width and height.
    Xf at;
    at.e = to_f(n.attr("x"));
    at.f = to_f(n.attr("y"));
    ctx.xf = ctx.xf.then(at);
    float vx = 0, vy = 0, vw = 0, vh = 0;
    const std::string vb = n.attr("viewbox");
    PathReader r(vb);
    if (!vb.empty() && r.next_num(vx) && r.next_num(vy) && r.next_num(vw) && r.next_num(vh) && vw > 0 && vh > 0)
      ctx.xf = ctx.xf.then(fit_viewbox(vx, vy, vw, vh, to_f(n.attr("width"), vw), to_f(n.attr("height"), vh),
                                       n.attr("preserveaspectratio")));
  }

  std::vector<Poly> polys = shape_of(n);
  if (!polys.empty()) {
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    for (const auto& poly : polys)
      for (const auto& q : poly.pts) {
        bx0 = std::min(bx0, q.x); by0 = std::min(by0, q.y);
        bx1 = std::max(bx1, q.x); by1 = std::max(by1, q.y);
      }
    for (auto& poly : polys)
      for (auto& q : poly.pts) q = ctx.xf.map(q);
    if (ctx.fill.has && t != "line" && t != "polyline") {
      fill_polys(cv, polys, lowered_trim(prop_of(n, "fill-rule")) == "evenodd");
      composite(cv, ctx.fill, ctx.fill_opacity * ctx.opacity, ctx, rd, bx0, by0, bx1, by1);
    }
    if (ctx.stroke.has && ctx.stroke_w > 0) {
      stroke_polys(cv, polys, ctx.stroke_w * ctx.xf.scale());
      composite(cv, ctx.stroke, ctx.stroke_opacity * ctx.opacity, ctx, rd, bx0, by0, bx1, by1);
    }
  }
  draw_children(cv, n, ctx, rd, depth);
}

}  // namespace

// The <svg> element, drawn into a bitmap `w` by `h`. `current` is the element's
// computed colour, which is what `currentColor` means and what every icon in a
// set like this is painted with.
Result<Bitmap> rasterise_svg(const Node& svg, int w, int h, uint32_t current) {
  if (w <= 0 || h <= 0) return Result<Bitmap>(Status::Invalid, "svg: no box to draw into");
  if (w > kMaxSide || h > kMaxSide) return Result<Bitmap>(Status::Invalid, "svg: larger than this draws");

  // The viewBox, or the element's own width/height, or the 24-unit grid every
  // icon set of this shape is drawn on.
  float vx = 0, vy = 0, vw = 0, vh = 0;
  // The markup parser folds attribute names to lower case, so it is `viewbox` by
  // the time it gets here; without this every drawing not made on a 24-unit grid
  // was scaled as if it were.
  const std::string vb = svg.attr("viewbox").empty() ? svg.attr("viewBox") : svg.attr("viewbox");
  if (!vb.empty()) {
    PathReader r(vb);
    if (!(r.next_num(vx) && r.next_num(vy) && r.next_num(vw) && r.next_num(vh))) { vw = vh = 0; }
  }
  if (vw <= 0 || vh <= 0) {
    vw = to_f(svg.attr("width"), 24);
    vh = to_f(svg.attr("height"), 24);
    vx = vy = 0;
  }
  if (vw <= 0 || vh <= 0) { vw = vh = 24; }

  Canvas cv;
  cv.w = w * kSS;
  cv.h = h * kSS;
  cv.px.assign((size_t)cv.w * cv.h, 0u);
  cv.mask.assign((size_t)cv.w * cv.h, 0u);

  Ctx ctx;
  ctx.current = current;
  ctx.fill.has = true;
  ctx.fill.color = current;               // SVG's initial fill is black; an icon
  ctx.stroke.has = false;                 // on a dark ground wants the ink colour
  ctx.stroke.color = current;
  ctx.stroke_w = 1.0f;
  // The viewBox into the box by `preserveAspectRatio` -- xMidYMid meet unless the drawing says otherwise, since an
  // icon squashed to a non-square box is worse than an icon with air around it.
  ctx.xf = fit_viewbox(vx, vy, vw, vh, (float)cv.w, (float)cv.h, svg.attr("preserveaspectratio"));
  Render rd;
  rd.root = &svg;
  rd.vw = vw;
  rd.vh = vh;
  // The <svg> element's OWN presentation attributes first. This walked straight
  // into the children, so `fill="none" stroke="currentColor"` -- which is where
  // every icon set of this shape puts them, once, on the root -- was never
  // read: a stroked outline came out as a solid blob, and a path with no fill
  // came out as nothing at all.
  draw_node(cv, svg, ctx, rd, 0);

  // Box-filter down: each output pixel is the average of its kSS^2 samples, weighted by their alpha (so a clear
  // sample does not darken the colour), which is the antialiasing.
  Bitmap out;
  out.w = w;
  out.h = h;
  out.pixels.assign((size_t)w * h, 0u);
  const int n = kSS * kSS;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint32_t r = 0, g = 0, b = 0, asum = 0;
      for (int sy2 = 0; sy2 < kSS; ++sy2)
        for (int sx2 = 0; sx2 < kSS; ++sx2) {
          const uint32_t p2 = cv.px[(size_t)(y * kSS + sy2) * cv.w + (x * kSS + sx2)];
          const uint32_t a = p2 >> 24;
          if (!a) continue;
          r += ((p2 >> 16) & 0xFF) * a; g += ((p2 >> 8) & 0xFF) * a; b += (p2 & 0xFF) * a;
          asum += a;
        }
      if (!asum) continue;
      const uint32_t a = asum / (uint32_t)n;
      if (!a) continue;
      out.pixels[(size_t)y * w + x] = (a << 24) | ((r / asum) << 16) | ((g / asum) << 8) | (b / asum);
    }
  }
  return Result<Bitmap>(std::move(out));
}

}  // namespace aios::userland::web
