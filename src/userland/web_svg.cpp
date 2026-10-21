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
// circle, ellipse, line, polyline, polygon, and <g> for the attributes it
// carries down. Fill is nonzero winding. Strokes are round-joined and
// round-capped, which is what every icon set of this shape asks for anyway.
#include <cstdlib>
#include <cmath>
#include <cstring>

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

struct Paint {
  bool has = false;
  uint32_t color = 0xFF000000;
};

struct Ctx {
  Paint fill;
  Paint stroke;
  float stroke_w = 1.0f;
};

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == ','; }

float to_f(const std::string& s, float dflt = 0.0f) {
  if (s.empty()) return dflt;
  return (float)std::atof(s.c_str());
}

// `none`, `currentColor`, `#rgb`, `#rrggbb`, or `rgb(...)`. Anything else is
// left to the caller's default rather than guessed at.
bool parse_paint(const std::string& raw, uint32_t current, Paint& out) {
  std::string v;
  for (char c : raw) if (!is_space(c) || c == ',') v += (c == ',' ? c : c);
  v.clear();
  for (char c : raw) { if (c != ' ' && c != '\t' && c != '\n' && c != '\r') v += c; }
  if (v.empty()) return false;
  std::string low;
  for (char c : v) low += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  if (low == "none" || low == "transparent") { out.has = false; return true; }
  if (low == "currentcolor" || low == "inherit") { out.has = true; out.color = current; return true; }
  if (v[0] == '#') {
    const std::string hex = v.substr(1);
    auto nib = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int r, g, b;
    if (hex.size() >= 6) {
      r = nib(hex[0]) * 16 + nib(hex[1]);
      g = nib(hex[2]) * 16 + nib(hex[3]);
      b = nib(hex[4]) * 16 + nib(hex[5]);
    } else if (hex.size() >= 3) {
      r = nib(hex[0]) * 17; g = nib(hex[1]) * 17; b = nib(hex[2]) * 17;
    } else {
      return false;
    }
    if (r < 0 || g < 0 || b < 0) return false;
    out.has = true;
    out.color = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    return true;
  }
  if (low.compare(0, 4, "rgb(") == 0 || low.compare(0, 5, "rgba(") == 0) {
    const size_t open = low.find('(');
    int v3[4] = {0, 0, 0, 255};
    int at = 0;
    size_t i = open + 1;
    while (i < low.size() && at < 4) {
      while (i < low.size() && (is_space(low[i]))) ++i;
      size_t j = i;
      while (j < low.size() && low[j] != ',' && low[j] != ')') ++j;
      v3[at++] = (int)to_f(low.substr(i, j - i));
      i = j + 1;
      if (j < low.size() && low[j] == ')') break;
    }
    out.has = true;
    out.color = 0xFF000000u | ((uint32_t)(v3[0] & 0xFF) << 16) | ((uint32_t)(v3[1] & 0xFF) << 8) |
                (uint32_t)(v3[2] & 0xFF);
    return true;
  }
  // A named colour this does not carry: treat as the current colour rather than
  // as black, which on a dark ground is invisible.
  out.has = true;
  out.color = current;
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

struct Canvas {
  int w = 0, h = 0;
  std::vector<uint32_t> px;               // ARGB, premultiplied by nothing: opaque or clear
  void set(int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    px[(size_t)y * w + x] = c;
  }
};

// Nonzero-winding scanline fill of a set of closed polygons.
void fill_polys(Canvas& cv, const std::vector<Poly>& polys, uint32_t color) {
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
      wind += xs[i].dir;
      if (wind == 0) continue;
      int xa = (int)std::ceil(xs[i].x - 0.5f);
      int xb = (int)std::ceil(xs[i + 1].x - 0.5f);
      if (xa < 0) xa = 0;
      if (xb > cv.w) xb = cv.w;
      for (int x = xa; x < xb; ++x) cv.set(x, y, color);
    }
  }
}

void fill_disc(Canvas& cv, float cx, float cy, float r, uint32_t color) {
  if (r <= 0) return;
  const int x0 = (int)std::floor(cx - r), x1 = (int)std::ceil(cx + r);
  const int y0 = (int)std::floor(cy - r), y1 = (int)std::ceil(cy + r);
  const float r2 = r * r;
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) {
      const float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
      if (dx * dx + dy * dy <= r2) cv.set(x, y, color);
    }
}

// A stroked polyline: each segment as a filled quad, each join and cap as a
// disc. Round joins and caps are what this icon shape asks for, and a disc is
// both of them.
void stroke_polys(Canvas& cv, const std::vector<Poly>& polys, float width, uint32_t color) {
  const float r = width / 2.0f;
  if (r <= 0.05f) return;
  for (const auto& p : polys) {
    const size_t n = p.pts.size();
    if (n == 1) { fill_disc(cv, p.pts[0].x, p.pts[0].y, r, color); continue; }
    for (size_t k = 0; k + 1 < n; ++k) {
      const Pt a = p.pts[k], b = p.pts[k + 1];
      const float dx = b.x - a.x, dy = b.y - a.y;
      const float len = std::sqrt(dx * dx + dy * dy);
      if (len < 0.0001f) { fill_disc(cv, a.x, a.y, r, color); continue; }
      const float nx = -dy / len * r, ny = dx / len * r;
      Poly quad;
      quad.pts = {{a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny}};
      quad.closed = true;
      std::vector<Poly> one{quad};
      fill_polys(cv, one, color);
      fill_disc(cv, a.x, a.y, r, color);
    }
    fill_disc(cv, p.pts[n - 1].x, p.pts[n - 1].y, r, color);
    if (p.closed && n > 2) {
      const Pt a = p.pts[n - 1], b = p.pts[0];
      const float dx = b.x - a.x, dy = b.y - a.y;
      const float len = std::sqrt(dx * dx + dy * dy);
      if (len > 0.0001f) {
        const float nx = -dy / len * r, ny = dx / len * r;
        Poly quad;
        quad.pts = {{a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny}};
        std::vector<Poly> one{quad};
        fill_polys(cv, one, color);
      }
    }
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

void draw_node(Canvas& cv, const Node& n, Ctx ctx, float sx, float sy, float ox, float oy, int depth) {
  if (depth > 24) return;
  if (n.kind != Node::Kind::Element) return;
  // These hold DEFINITIONS -- a clip region, a gradient, a mask, a symbol to be
  // <use>d -- not pictures. Drawn as if they were, the full-frame `<path
  // fill="#fff">` inside a <clipPath> went over the whole drawing: Wikipedia's
  // wordmark came out solid white on a white page.
  {
    const std::string& t = n.tag;
    if (t == "defs" || t == "clippath" || t == "clipPath" || t == "mask" || t == "symbol" ||
        t == "lineargradient" || t == "linearGradient" || t == "radialgradient" || t == "radialGradient" ||
        t == "pattern" || t == "marker" || t == "style" || t == "title" || t == "desc" ||
        t == "metadata" || t == "filter" || t == "script")
      return;
    const std::string d = n.attr("display");
    if (d == "none") return;
  }

  // Presentation attributes carry down through <g> exactly as they do in a
  // stylesheet, which is how an icon set puts stroke="currentColor" once on the
  // <svg> and never mentions it again.
  Paint p;
  if (n.has_attr("fill") && parse_paint(n.attr("fill"), ctx.fill.color, p)) ctx.fill = p;
  if (n.has_attr("stroke") && parse_paint(n.attr("stroke"), ctx.stroke.color, p)) ctx.stroke = p;
  if (n.has_attr("stroke-width")) {
    const float w = to_f(n.attr("stroke-width"), ctx.stroke_w);
    if (w > 0) ctx.stroke_w = w;
  }

  std::vector<Poly> polys = shape_of(n);
  if (!polys.empty()) {
    for (auto& poly : polys)
      for (auto& q : poly.pts) { q.x = q.x * sx + ox; q.y = q.y * sy + oy; }
    if (ctx.fill.has) fill_polys(cv, polys, ctx.fill.color);
    if (ctx.stroke.has) stroke_polys(cv, polys, ctx.stroke_w * (sx < sy ? sx : sy), ctx.stroke.color);
  }
  for (const auto& kid : n.children) if (kid) draw_node(cv, *kid, ctx, sx, sy, ox, oy, depth + 1);
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

  // Fit the viewBox into the box, preserving the aspect ratio and centring --
  // `preserveAspectRatio` defaults to xMidYMid meet, and an icon squashed to a
  // non-square box is worse than an icon with air around it.
  const float sx0 = (float)cv.w / vw, sy0 = (float)cv.h / vh;
  const float s = sx0 < sy0 ? sx0 : sy0;
  const float ox = ((float)cv.w - vw * s) / 2.0f - vx * s;
  const float oy = ((float)cv.h - vh * s) / 2.0f - vy * s;

  Ctx ctx;
  ctx.fill.has = true;
  ctx.fill.color = current;               // SVG's initial fill is black; an icon
  ctx.stroke.has = false;                 // on a dark ground wants the ink colour
  ctx.stroke.color = current;
  ctx.stroke_w = 1.0f;
  // The <svg> element's OWN presentation attributes first. This walked straight
  // into the children, so `fill="none" stroke="currentColor"` -- which is where
  // every icon set of this shape puts them, once, on the root -- was never
  // read: a stroked outline came out as a solid blob, and a path with no fill
  // came out as nothing at all.
  draw_node(cv, svg, ctx, s, s, ox, oy, 0);

  // Box-filter down: the coverage of each output pixel is how many of its kSS^2
  // samples were painted, which is the antialiasing.
  Bitmap out;
  out.w = w;
  out.h = h;
  out.pixels.assign((size_t)w * h, 0u);
  const int n = kSS * kSS;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint32_t r = 0, g = 0, b = 0, hits = 0;
      for (int sy2 = 0; sy2 < kSS; ++sy2)
        for (int sx2 = 0; sx2 < kSS; ++sx2) {
          const uint32_t p2 = cv.px[(size_t)(y * kSS + sy2) * cv.w + (x * kSS + sx2)];
          if (p2 >> 24) { r += (p2 >> 16) & 0xFF; g += (p2 >> 8) & 0xFF; b += p2 & 0xFF; ++hits; }
        }
      if (!hits) continue;
      const uint32_t a = (uint32_t)(255 * (int)hits / n);
      out.pixels[(size_t)y * w + x] =
          (a << 24) | ((r / hits) << 16) | ((g / hits) << 8) | (b / hits);
    }
  }
  return Result<Bitmap>(std::move(out));
}

}  // namespace aios::userland::web
