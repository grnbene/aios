// A TrueType reader and a scanline rasteriser, so the machine can cut a glyph
// it was never shipped. See include/aios/userland/glyph_engine.hpp for why.
//
// Nothing here calls a font library. `glyf` outlines are quadratic B-splines;
// reading them is table walking, and filling them is a sorted-crossing scan
// with a nonzero winding rule. That is the whole engine, and it is the reason
// this can live in a freestanding image as readily as in a hosted one.
#include "aios/userland/glyph_engine.hpp"
#include "aios/hal/sync.hpp"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if !defined(AIOS_METAL)
#include <cstdlib>
#include <fstream>
#endif

namespace aios::userland::glyphs {
namespace {

// ---------------------------------------------------------------- byte order
// Everything in an sfnt is big-endian, whatever the machine reading it is.
inline uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline int16_t rds16(const uint8_t* p) { return static_cast<int16_t>(rd16(p)); }
inline uint32_t rd32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// -------------------------------------------------------------------- arith
// Written out rather than taken from <cmath>: fminf/fmaxf live in libm, and
// the bare-metal image links no libm. Every value here is a glyph coordinate,
// so the ranges are small and the arithmetic is exact.
inline float f_abs(float v) { return v < 0 ? -v : v; }
inline float f_min(float a, float b) { return a < b ? a : b; }
inline float f_max(float a, float b) { return a > b ? a : b; }
inline int f_floor(float v) {
  int i = static_cast<int>(v);
  return (v < 0 && static_cast<float>(i) != v) ? i - 1 : i;
}
inline int f_ceil(float v) {
  int i = static_cast<int>(v);
  return (v > 0 && static_cast<float>(i) != v) ? i + 1 : i;
}

// ------------------------------------------------------------------ geometry
struct Pt {
  float x = 0, y = 0;
};

// An outline edge in device pixels, y already flipped to grow downward. The
// rasteriser never sees a curve: everything is flattened on the way in.
struct Edge {
  float x0, y0, x1, y1;
};

// A component's placement inside a composite glyph. Identity for a simple one.
struct Xform {
  float a = 1, b = 0, c = 0, d = 1, dx = 0, dy = 0;
  Pt apply(float x, float y) const { return {a * x + c * y + dx, b * x + d * y + dy}; }
};

// -------------------------------------------------------------- the sfnt file
// One typeface's bytes plus the table slices worth keeping. A font is parsed
// once and then only read, so every pointer below is into `bytes`.
struct FontFile {
  std::vector<uint8_t> bytes;
  std::string name;

  const uint8_t* glyf = nullptr;
  size_t glyf_len = 0;
  const uint8_t* loca = nullptr;
  size_t loca_len = 0;
  const uint8_t* hmtx = nullptr;
  size_t hmtx_len = 0;
  const uint8_t* cmap_sub = nullptr;
  size_t cmap_len = 0;

  uint16_t num_glyphs = 0;
  uint16_t num_hmetrics = 0;
  uint16_t upem = 1000;
  uint16_t cmap_fmt = 0;
  bool loca_long = false;
  bool ok = false;

  const uint8_t* table(const char* tag, size_t* len) const;
  bool parse(size_t collection_index);
  bool pick_cmap(const uint8_t* cm, size_t len);
  uint16_t glyph_index(uint32_t cp) const;
  uint16_t advance(uint16_t gid) const;
  bool glyph_range(uint16_t gid, size_t* off, size_t* len) const;
  // Appends this glyph's edges, already scaled and y-flipped, to `out`.
  void emit(uint16_t gid, const Xform& t, float scale, std::vector<Edge>& out, int depth) const;
};

const uint8_t* FontFile::table(const char* tag, size_t* len) const {
  // The offset table was validated in parse(); this only re-walks the record
  // array, which is 16 bytes per table starting at the directory head.
  if (bytes.size() < 12) return nullptr;
  size_t base = 0;
  if (rd32(bytes.data()) == 0x74746366u) {  // 'ttcf': a collection header
    if (bytes.size() < 16) return nullptr;
    base = rd32(bytes.data() + 12);
    if (base + 12 > bytes.size()) return nullptr;
  }
  uint16_t n = rd16(bytes.data() + base + 4);
  for (uint16_t i = 0; i < n; ++i) {
    size_t rec = base + 12 + static_cast<size_t>(i) * 16;
    if (rec + 16 > bytes.size()) return nullptr;
    if (std::memcmp(bytes.data() + rec, tag, 4) != 0) continue;
    size_t off = rd32(bytes.data() + rec + 8);
    size_t sz = rd32(bytes.data() + rec + 12);
    if (off > bytes.size()) return nullptr;
    if (sz > bytes.size() - off) sz = bytes.size() - off;  // some fonts overstate
    *len = sz;
    return bytes.data() + off;
  }
  return nullptr;
}

// Which cmap subtable to read. A full Unicode one is worth having over a BMP
// one, because it is the only kind that can name anything past U+FFFF.
bool FontFile::pick_cmap(const uint8_t* cm, size_t len) {
  if (!cm || len < 4) return false;
  uint16_t n = rd16(cm + 2);
  const uint8_t* best = nullptr;
  int best_rank = -1;
  for (uint16_t i = 0; i < n; ++i) {
    size_t rec = 4 + static_cast<size_t>(i) * 8;
    if (rec + 8 > len) break;
    uint16_t plat = rd16(cm + rec), enc = rd16(cm + rec + 2);
    size_t off = rd32(cm + rec + 4);
    if (off + 4 > len) continue;
    uint16_t fmt = rd16(cm + off);
    int rank = -1;
    if (plat == 3 && enc == 10 && fmt == 12) rank = 4;        // Windows, full Unicode
    else if (plat == 0 && fmt == 12) rank = 3;                // Unicode, full
    else if (plat == 3 && enc == 1 && fmt == 4) rank = 2;     // Windows, BMP
    else if (plat == 0 && fmt == 4) rank = 1;                 // Unicode, BMP
    else if (plat == 1 && enc == 0 && fmt == 0) rank = 0;     // Mac Roman, last resort
    if (rank > best_rank) {
      best_rank = rank;
      best = cm + off;
      cmap_fmt = fmt;
      cmap_len = len - off;
    }
  }
  cmap_sub = best;
  return best != nullptr;
}

bool FontFile::parse(size_t collection_index) {
  if (bytes.size() < 12) return false;
  size_t base = 0;
  if (rd32(bytes.data()) == 0x74746366u) {
    if (bytes.size() < 16) return false;
    uint32_t count = rd32(bytes.data() + 8);
    if (collection_index >= count) return false;
    size_t rec = 12 + collection_index * 4;
    if (rec + 4 > bytes.size()) return false;
    base = rd32(bytes.data() + rec);
  }
  if (base + 12 > bytes.size()) return false;
  uint32_t ver = rd32(bytes.data() + base);
  // 0x00010000 and 'true' are outline fonts we can read. 'OTTO' is CFF: real
  // curves, a completely different charstring interpreter, and not this file.
  if (ver != 0x00010000u && ver != 0x74727565u) return false;

  size_t len = 0;
  const uint8_t* head = table("head", &len);
  if (!head || len < 54) return false;
  upem = rd16(head + 18);
  if (upem == 0) return false;
  loca_long = rd16(head + 50) != 0;

  const uint8_t* maxp = table("maxp", &len);
  if (!maxp || len < 6) return false;
  num_glyphs = rd16(maxp + 4);

  const uint8_t* hhea = table("hhea", &len);
  if (!hhea || len < 36) return false;
  num_hmetrics = rd16(hhea + 34);

  hmtx = table("hmtx", &hmtx_len);
  loca = table("loca", &loca_len);
  glyf = table("glyf", &glyf_len);
  if (!loca || !glyf) return false;  // no outlines here to cut

  size_t cmlen = 0;
  const uint8_t* cm = table("cmap", &cmlen);
  if (!pick_cmap(cm, cmlen)) return false;

  ok = true;
  return true;
}

uint16_t FontFile::glyph_index(uint32_t cp) const {
  const uint8_t* t = cmap_sub;
  if (!t) return 0;
  if (cmap_fmt == 4) {
    if (cmap_len < 14) return 0;
    if (cp > 0xFFFF) return 0;
    uint16_t segx2 = rd16(t + 6);
    if (segx2 == 0 || cmap_len < 16u + static_cast<size_t>(segx2) * 4) return 0;
    const uint8_t* ends = t + 14;
    const uint8_t* starts = ends + segx2 + 2;
    const uint8_t* deltas = starts + segx2;
    const uint8_t* ranges = deltas + segx2;
    for (uint16_t s = 0; s < segx2; s += 2) {
      if (rd16(ends + s) < cp) continue;
      if (rd16(starts + s) > cp) return 0;
      uint16_t ro = rd16(ranges + s);
      if (ro == 0) return static_cast<uint16_t>(cp + rd16(deltas + s));
      const uint8_t* p = ranges + s + ro + 2 * (cp - rd16(starts + s));
      if (p + 2 > t + cmap_len) return 0;
      uint16_t g = rd16(p);
      return g ? static_cast<uint16_t>(g + rd16(deltas + s)) : 0;
    }
    return 0;
  }
  if (cmap_fmt == 12) {
    if (cmap_len < 16) return 0;
    uint32_t n = rd32(t + 12);
    // Sorted by start code, so this is a binary search and not a walk of a
    // table that in a CJK font has thousands of groups.
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
      uint32_t mid = lo + (hi - lo) / 2;
      size_t rec = 16 + static_cast<size_t>(mid) * 12;
      if (rec + 12 > cmap_len) return 0;
      uint32_t s = rd32(t + rec), e = rd32(t + rec + 4);
      if (cp < s) hi = mid;
      else if (cp > e) lo = mid + 1;
      else return static_cast<uint16_t>(rd32(t + rec + 8) + (cp - s));
    }
    return 0;
  }
  if (cmap_fmt == 0) {
    if (cp > 255 || cmap_len < 262) return 0;
    return t[6 + cp];
  }
  return 0;
}

uint16_t FontFile::advance(uint16_t gid) const {
  if (!hmtx || num_hmetrics == 0) return 0;
  // Past the last full metric every glyph shares that metric's advance; only
  // the left side bearing keeps varying, and we do not use it.
  uint16_t i = gid < num_hmetrics ? gid : static_cast<uint16_t>(num_hmetrics - 1);
  size_t off = static_cast<size_t>(i) * 4;
  if (off + 2 > hmtx_len) return 0;
  return rd16(hmtx + off);
}

bool FontFile::glyph_range(uint16_t gid, size_t* off, size_t* len) const {
  if (gid >= num_glyphs) return false;
  size_t a, b;
  if (loca_long) {
    if (static_cast<size_t>(gid) * 4 + 8 > loca_len) return false;
    a = rd32(loca + static_cast<size_t>(gid) * 4);
    b = rd32(loca + static_cast<size_t>(gid) * 4 + 4);
  } else {
    if (static_cast<size_t>(gid) * 2 + 4 > loca_len) return false;
    a = static_cast<size_t>(rd16(loca + static_cast<size_t>(gid) * 2)) * 2;
    b = static_cast<size_t>(rd16(loca + static_cast<size_t>(gid) * 2 + 2)) * 2;
  }
  if (b <= a || b > glyf_len) return false;  // b == a is a blank glyph, e.g. space
  *off = a;
  *len = b - a;
  return true;
}

// How many line segments a quadratic gets. Long curves need more; a curve two
// pixels across needs three and looks the same with thirty.
int flatten_steps(const Pt& p0, const Pt& p1, const Pt& p2) {
  float d = f_abs(p1.x - p0.x) + f_abs(p1.y - p0.y) + f_abs(p2.x - p1.x) +
            f_abs(p2.y - p1.y);
  int n = static_cast<int>(d * 0.6f);
  if (n < 3) n = 3;
  if (n > 24) n = 24;
  return n;
}

void push_edge(std::vector<Edge>& out, const Pt& a, const Pt& b) {
  if (a.y == b.y) return;  // a horizontal edge crosses no scanline
  out.push_back({a.x, a.y, b.x, b.y});
}

void push_quad(std::vector<Edge>& out, const Pt& p0, const Pt& ctrl, const Pt& p2) {
  int n = flatten_steps(p0, ctrl, p2);
  Pt prev = p0;
  for (int i = 1; i <= n; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(n);
    float u = 1.0f - t;
    Pt p{u * u * p0.x + 2 * u * t * ctrl.x + t * t * p2.x,
         u * u * p0.y + 2 * u * t * ctrl.y + t * t * p2.y};
    push_edge(out, prev, p);
    prev = p;
  }
}

void FontFile::emit(uint16_t gid, const Xform& t, float scale, std::vector<Edge>& out,
                    int depth) const {
  if (depth > 4) return;  // a composite that deep is a malformed font
  size_t off = 0, len = 0;
  if (!glyph_range(gid, &off, &len) || len < 10) return;
  const uint8_t* g = glyf + off;
  int16_t ncont = rds16(g);

  if (ncont < 0) {
    // Composite: a list of other glyphs, each with its own placement. This is
    // how every accented letter in a real font is built, so Ä depends on it.
    size_t p = 10;
    for (;;) {
      if (p + 4 > len) return;
      uint16_t flags = rd16(g + p), sub = rd16(g + p + 2);
      p += 4;
      float dx = 0, dy = 0;
      if (flags & 0x0001) {
        if (p + 4 > len) return;
        if (flags & 0x0002) { dx = rds16(g + p); dy = rds16(g + p + 2); }
        p += 4;
      } else {
        if (p + 2 > len) return;
        if (flags & 0x0002) {
          dx = static_cast<int8_t>(g[p]);
          dy = static_cast<int8_t>(g[p + 1]);
        }
        p += 2;
      }
      Xform c;
      auto f2dot14 = [](const uint8_t* q) { return rds16(q) / 16384.0f; };
      if (flags & 0x0008) {
        if (p + 2 > len) return;
        c.a = c.d = f2dot14(g + p);
        p += 2;
      } else if (flags & 0x0040) {
        if (p + 4 > len) return;
        c.a = f2dot14(g + p);
        c.d = f2dot14(g + p + 2);
        p += 4;
      } else if (flags & 0x0080) {
        if (p + 8 > len) return;
        c.a = f2dot14(g + p);
        c.b = f2dot14(g + p + 2);
        c.c = f2dot14(g + p + 4);
        c.d = f2dot14(g + p + 6);
        p += 8;
      }
      c.dx = dx;
      c.dy = dy;
      // Compose with the parent's placement, so a component of a component
      // still lands where the outermost glyph says it should.
      Xform n;
      n.a = c.a * t.a + c.b * t.c;
      n.b = c.a * t.b + c.b * t.d;
      n.c = c.c * t.a + c.d * t.c;
      n.d = c.c * t.b + c.d * t.d;
      n.dx = c.dx * t.a + c.dy * t.c + t.dx;
      n.dy = c.dx * t.b + c.dy * t.d + t.dy;
      emit(sub, n, scale, out, depth + 1);
      if (!(flags & 0x0020)) break;  // MORE_COMPONENTS
    }
    return;
  }

  // Simple glyph: contour ends, then hint bytecode we skip, then a flag run,
  // then x deltas, then y deltas. The three arrays are variable length and
  // only readable in order, which is why this is a walk and not an index.
  size_t p = 10;
  if (p + static_cast<size_t>(ncont) * 2 + 2 > len) return;
  std::vector<uint16_t> ends(static_cast<size_t>(ncont));
  for (int i = 0; i < ncont; ++i) ends[i] = rd16(g + p + static_cast<size_t>(i) * 2);
  p += static_cast<size_t>(ncont) * 2;
  uint16_t npts = ncont ? static_cast<uint16_t>(ends[ncont - 1] + 1) : 0;
  if (npts == 0) return;
  uint16_t ilen = rd16(g + p);
  p += 2 + ilen;
  if (p > len) return;

  std::vector<uint8_t> flags(npts);
  for (uint16_t i = 0; i < npts;) {
    if (p >= len) return;
    uint8_t f = g[p++];
    flags[i++] = f;
    if (f & 0x08) {  // REPEAT
      if (p >= len) return;
      uint8_t r = g[p++];
      while (r-- && i < npts) flags[i++] = f;
    }
  }

  std::vector<int32_t> xs(npts), ys(npts);
  int32_t v = 0;
  for (uint16_t i = 0; i < npts; ++i) {
    uint8_t f = flags[i];
    if (f & 0x02) {
      if (p >= len) return;
      int32_t d = g[p++];
      v += (f & 0x10) ? d : -d;
    } else if (!(f & 0x10)) {
      if (p + 2 > len) return;
      v += rds16(g + p);
      p += 2;
    }
    xs[i] = v;
  }
  v = 0;
  for (uint16_t i = 0; i < npts; ++i) {
    uint8_t f = flags[i];
    if (f & 0x04) {
      if (p >= len) return;
      int32_t d = g[p++];
      v += (f & 0x20) ? d : -d;
    } else if (!(f & 0x20)) {
      if (p + 2 > len) return;
      v += rds16(g + p);
      p += 2;
    }
    ys[i] = v;
  }

  // Font units to device pixels, with y flipped so the rasteriser's rows run
  // down the way a framebuffer's do.
  auto dev = [&](uint16_t i) {
    Pt q = t.apply(static_cast<float>(xs[i]), static_cast<float>(ys[i]));
    return Pt{q.x * scale, -q.y * scale};
  };

  uint16_t start = 0;
  for (int ci = 0; ci < ncont; ++ci) {
    uint16_t end = ends[ci];
    if (end < start || end >= npts) { start = static_cast<uint16_t>(end + 1); continue; }
    uint16_t n = static_cast<uint16_t>(end - start + 1);
    if (n < 2) { start = static_cast<uint16_t>(end + 1); continue; }

    auto on = [&](uint16_t k) { return (flags[start + (k % n)] & 0x01) != 0; };
    auto pt = [&](uint16_t k) { return dev(static_cast<uint16_t>(start + (k % n))); };
    // Two off-curve points in a row imply an on-curve point at their midpoint.
    auto mid = [](const Pt& a, const Pt& b) { return Pt{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f}; };

    // Find a point to start from that is actually on the curve; if the contour
    // is all off-curve, start from an implied midpoint.
    uint16_t first = 0;
    bool found = false;
    for (uint16_t k = 0; k < n; ++k) {
      if (on(k)) { first = k; found = true; break; }
    }
    Pt cur = found ? pt(first) : mid(pt(0), pt(1));
    const Pt open = cur;
    uint16_t k = static_cast<uint16_t>(found ? first + 1 : 1);
    for (uint16_t step = 0; step < n; ++step, ++k) {
      Pt a = pt(k);
      if (on(k)) {
        push_edge(out, cur, a);
        cur = a;
      } else {
        Pt next = pt(static_cast<uint16_t>(k + 1));
        Pt endp = on(static_cast<uint16_t>(k + 1)) ? next : mid(a, next);
        push_quad(out, cur, a, endp);
        cur = endp;
        if (!on(static_cast<uint16_t>(k + 1))) continue;  // the implied point is not a real one
        ++k;
        ++step;
      }
    }
    push_edge(out, cur, open);
    start = static_cast<uint16_t>(end + 1);
  }
}

// ---------------------------------------------------------------- the filler
// Sub-scanlines per pixel row. Eight is enough that a diagonal stem's edge is
// smooth, and a glyph is filled once and then cached forever, so the cost is
// paid on the first character of its kind and never again.
constexpr int kSubSamples = 8;

void add_span(std::vector<float>& row, float xa, float xb, float weight, int w) {
  if (xb <= xa) return;
  if (xa < 0) xa = 0;
  if (xb > static_cast<float>(w)) xb = static_cast<float>(w);
  if (xb <= xa) return;
  int ia = static_cast<int>(xa), ib = static_cast<int>(xb);
  if (ib >= w) ib = w - 1;
  if (ia == ib) {
    row[ia] += (xb - xa) * weight;
    return;
  }
  row[ia] += (static_cast<float>(ia + 1) - xa) * weight;
  for (int i = ia + 1; i < ib; ++i) row[i] += weight;
  row[ib] += (xb - static_cast<float>(ib)) * weight;
}

// Nonzero winding fill. For each sub-scanline: every edge it crosses gives an
// x and a direction; sort them, walk left to right, and the spans where the
// winding count is not zero are inside the glyph.
void fill(const std::vector<Edge>& edges, int w, int h, std::vector<uint8_t>& out) {
  out.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
  std::vector<float> row(static_cast<size_t>(w));
  std::vector<std::pair<float, int>> xs;
  const float weight = 1.0f / kSubSamples;
  for (int py = 0; py < h; ++py) {
    std::fill(row.begin(), row.end(), 0.0f);
    for (int s = 0; s < kSubSamples; ++s) {
      float y = static_cast<float>(py) + (static_cast<float>(s) + 0.5f) / kSubSamples;
      xs.clear();
      for (const Edge& e : edges) {
        float y0 = e.y0, y1 = e.y1;
        int dir = 1;
        float x0 = e.x0, x1 = e.x1;
        if (y0 > y1) {
          std::swap(y0, y1);
          std::swap(x0, x1);
          dir = -1;
        }
        // Half-open in y, so a vertex shared by two edges is counted once.
        if (y < y0 || y >= y1) continue;
        float tt = (y - y0) / (y1 - y0);
        xs.push_back({x0 + tt * (x1 - x0), dir});
      }
      if (xs.size() < 2) continue;
      std::sort(xs.begin(), xs.end(),
                [](const std::pair<float, int>& a, const std::pair<float, int>& b) {
                  return a.first < b.first;
                });
      int wind = 0;
      for (size_t i = 0; i + 1 < xs.size(); ++i) {
        wind += xs[i].second;
        if (wind != 0) add_span(row, xs[i].first, xs[i + 1].first, weight, w);
      }
    }
    uint8_t* dst = out.data() + static_cast<size_t>(py) * static_cast<size_t>(w);
    for (int px = 0; px < w; ++px) {
      float c = row[px] * 255.0f + 0.5f;
      dst[px] = c <= 0 ? 0 : (c >= 255.0f ? 255 : static_cast<uint8_t>(c));
    }
  }
}

// ------------------------------------------------------------- which typeface
// The atlas faces are twelve named objects, and a face is identified by its
// address. Each one records which TTF and which pixel size it was cut at in
// font_atlas.hpp's comments; this is the same table, as code, so a runtime
// glyph comes off the SAME typeface as its neighbours in the baked block.
enum class Style { Sans, SansBold, SansOblique, SansBoldOblique, Mono, Count };

Style style_of(const fonts::Face& f) {
  const fonts::Face* p = &f;
  if (p == &fonts::UI_BOLD || p == &fonts::UI_BOLD_21 || p == &fonts::UI_BOLD_30 ||
      p == &fonts::UI_SMALL)
    return Style::SansBold;
  if (p == &fonts::UI_ITALIC || p == &fonts::UI_ITALIC_21) return Style::SansOblique;
  if (p == &fonts::UI_BOLD_ITALIC) return Style::SansBoldOblique;
  if (p == &fonts::MONO || p == &fonts::MONO_21) return Style::Mono;
  return Style::Sans;
}

// A typeface this style may draw from. The path is recorded at search time and
// the file is only READ when a lookup gets far enough down the chain to need
// it — a machine that never types Han never pays for the CJK font, which is
// several megabytes of it.
struct Ref {
  std::string path;
  FontFile* f = nullptr;
  bool tried = false;
  bool embedded = false;
};

struct Slot {
  std::vector<Ref> chain;  // the styled face first, then coverage fallbacks
};

struct Cached {
  Rasterized r{};
  std::vector<uint8_t> cov;
  bool hit = false;
};

struct Engine {
  aios::hal::Mutex mu;
  std::vector<FontFile*> fonts_held;
  std::vector<std::string> loaded_paths;
  Slot slots[static_cast<int>(Style::Count)];
  std::unordered_map<uint64_t, Cached> cache;
  std::string names;
  size_t misses = 0;
  size_t cov_bytes = 0;
  bool searched = false;
};

Engine& engine() {
  static Engine e;
  return e;
}

FontFile* adopt(std::vector<uint8_t>&& bytes, const char* name, size_t index) {
  FontFile* f = new FontFile();
  f->bytes = std::move(bytes);
  f->name = name ? name : "?";
  if (!f->parse(index)) {
    delete f;
    return nullptr;
  }
  return f;
}

#if !defined(AIOS_METAL)
// Where a hosted machine keeps its outlines. The styled entries come first so
// a Greek omega is cut from the same DejaVu the Latin around it came from; the
// wide-coverage faces are appended to every chain, because they are the only
// reason a code point outside a Latin font can be drawn at all.
struct Candidate {
  Style style;
  const char* path;
};

const Candidate kStyled[] = {
    {Style::Sans, "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"},
    {Style::Sans, "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"},
    {Style::Sans, "/System/Library/Fonts/Supplemental/Arial.ttf"},
    {Style::SansBold, "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"},
    {Style::SansBold, "/System/Library/Fonts/Supplemental/Arial Bold.ttf"},
    {Style::SansOblique, "/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf"},
    {Style::SansOblique, "/System/Library/Fonts/Supplemental/Arial Italic.ttf"},
    {Style::SansBoldOblique, "/usr/share/fonts/truetype/dejavu/DejaVuSans-BoldOblique.ttf"},
    {Style::SansBoldOblique, "/System/Library/Fonts/Supplemental/Arial Bold Italic.ttf"},
    {Style::Mono, "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"},
    {Style::Mono, "/System/Library/Fonts/Supplemental/Andale Mono.ttf"},
    {Style::Mono, "/System/Library/Fonts/Menlo.ttc"},
};

// Tried in order, and every one that loads is appended to every style's chain.
// Breadth beats matching weight here: a reader would rather see a Han
// character in the wrong typeface than a box in the right one.
const char* const kFallbacks[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansMath-Regular.ttf",
    // The CJK font on a Debian box that is actually readable here. Noto's CJK
    // ships as OpenType/CFF, whose outlines are charstrings rather than glyf,
    // so the parser above declines it; Droid's fallback is TrueType and
    // carries the same Han, Kana and Hangul.
    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansHebrew-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansDevanagari-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansThai-Regular.ttf",
    "/usr/share/fonts/truetype/unifont/unifont.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
    "/System/Library/Fonts/Apple Symbols.ttf",
    "/System/Library/Fonts/Supplemental/Symbol.ttf",
    "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
    "/System/Library/Fonts/Geneva.ttf",
};

// stdio, not ifstream: the same everywhere, including WebAssembly builds (whose libc++ has no fstream)
bool slurp(const char* path, std::vector<uint8_t>& out) {
  FILE* in = std::fopen(path, "rb");
  if (!in) return false;
  long n = (std::fseek(in, 0, SEEK_END) == 0) ? std::ftell(in) : -1;
  if (n <= 0 || n > (64 << 20) || std::fseek(in, 0, SEEK_SET) != 0) { std::fclose(in); return false; }
  out.resize(static_cast<size_t>(n));
  const bool ok = std::fread(out.data(), 1, out.size(), in) == out.size();
  std::fclose(in);
  return ok;
}

bool file_exists(const char* path) {
  FILE* in = std::fopen(path, "rb");
  if (in) std::fclose(in);
  return in != nullptr;
}

void add_ref(Slot& s, const std::string& path) {
  for (const Ref& r : s.chain)
    if (r.path == path) return;
  Ref r;
  r.path = path;
  s.chain.push_back(r);
}

void search_host(Engine& e) {
  // Only names are collected here. Nothing is opened until a lookup walks far
  // enough down a chain to need it.
  //
  // One directory name is enough to move the whole search somewhere else,
  // which is what makes this testable against a font the machine does not
  // normally have.
  if (const char* dir = std::getenv("AIOS_FONT_DIR")) {
    static const char* const kNames[] = {"DejaVuSans.ttf", "DejaVuSans-Bold.ttf",
                                         "DejaVuSans-Oblique.ttf", "DejaVuSans-BoldOblique.ttf",
                                         "DejaVuSansMono.ttf"};
    static const Style kStyles[] = {Style::Sans, Style::SansBold, Style::SansOblique,
                                    Style::SansBoldOblique, Style::Mono};
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
      std::string p = std::string(dir) + "/" + kNames[i];
      if (file_exists(p.c_str())) add_ref(e.slots[static_cast<int>(kStyles[i])], p);
    }
  }
  for (const Candidate& c : kStyled) {
    Slot& s = e.slots[static_cast<int>(c.style)];
    if (!s.chain.empty()) continue;  // the first that is there wins its style
    if (file_exists(c.path)) add_ref(s, c.path);
  }
  if (const char* extra = std::getenv("AIOS_FONT_FALLBACK")) {
    if (file_exists(extra))
      for (Slot& s : e.slots) add_ref(s, extra);
  }
  for (const char* p : kFallbacks) {
    if (!file_exists(p)) continue;
    for (Slot& s : e.slots) add_ref(s, p);
  }
}
#endif  // !AIOS_METAL

// Read and parse a reference the first time something needs it. A file that
// turns out not to be a TrueType outline font — a CFF OpenType, a bitmap
// collection — is marked tried and skipped from then on.
FontFile* resolve(Engine& e, Ref& r) {
  if (r.tried) return r.f;
  r.tried = true;
#if !defined(AIOS_METAL)
  for (FontFile* h : e.fonts_held) {
    if (h->name == r.path) { r.f = h; return r.f; }  // named by two chains, read once
  }
  std::vector<uint8_t> bytes;
  if (!slurp(r.path.c_str(), bytes)) return nullptr;
  FontFile* f = adopt(std::move(bytes), r.path.c_str(), 0);
  if (!f) return nullptr;
  e.fonts_held.push_back(f);
  e.loaded_paths.push_back(r.path);
  e.names += r.path;
  e.names += '\n';
  r.f = f;
#else
  (void)e;
#endif
  return r.f;
}

void ensure_searched(Engine& e) {
  if (e.searched) return;
  e.searched = true;
#if !defined(AIOS_METAL)
  search_host(e);
#endif
}

// Cut one glyph from one font. Returns false when the font has no outline for
// the code point, which is the signal to try the next font in the chain.
bool cut(const FontFile& f, uint32_t cp, const fonts::Face& face, Cached& out) {
  uint16_t gid = f.glyph_index(cp);
  if (gid == 0) return false;  // .notdef: the font does not have it
  const float scale = static_cast<float>(face.px) / static_cast<float>(f.upem);
  std::vector<Edge> edges;
  f.emit(gid, Xform{}, scale, edges, 0);

  int adv = static_cast<int>(f.advance(gid) * scale + 0.5f);
  if (adv < 0) adv = 0;
  if (adv > 255) return false;

  if (edges.empty()) {
    // A real glyph with no ink: a space, or one of the fixed-width blanks.
    out.r.g = fonts::Glyph{0, 0, 0, 0, 0, static_cast<uint8_t>(adv)};
    out.cov.clear();
    out.r.cov = nullptr;
    out.hit = true;
    return true;
  }

  float minx = edges[0].x0, maxx = minx, miny = edges[0].y0, maxy = miny;
  for (const Edge& e : edges) {
    minx = f_min(minx, f_min(e.x0, e.x1));
    maxx = f_max(maxx, f_max(e.x0, e.x1));
    miny = f_min(miny, f_min(e.y0, e.y1));
    maxy = f_max(maxy, f_max(e.y0, e.y1));
  }
  int x0 = f_floor(minx);
  int y0 = f_floor(miny);
  int w = f_ceil(maxx) - x0;
  int h = f_ceil(maxy) - y0;
  if (w <= 0 || h <= 0) return false;
  // The Glyph struct holds the box in a byte and the bearings in a signed one.
  // A glyph that will not fit is refused rather than drawn at a wrong offset.
  if (w > 255 || h > 255) return false;
  int by = static_cast<int>(face.ascent) + y0;
  if (x0 < -128 || x0 > 127 || by < -128 || by > 127) return false;

  for (Edge& e : edges) {
    e.x0 -= static_cast<float>(x0);
    e.x1 -= static_cast<float>(x0);
    e.y0 -= static_cast<float>(y0);
    e.y1 -= static_cast<float>(y0);
  }
  fill(edges, w, h, out.cov);
  out.r.g = fonts::Glyph{0,
                         static_cast<uint8_t>(w),
                         static_cast<uint8_t>(h),
                         static_cast<int8_t>(x0),
                         static_cast<int8_t>(by),
                         static_cast<uint8_t>(adv)};
  out.r.cov = out.cov.data();
  out.hit = true;
  return true;
}

}  // namespace

const Rasterized* rasterize(const fonts::Face& face, uint32_t cp) {
  if (cp < 32 || cp > 0x10FFFF) return nullptr;
  Engine& e = engine();
  std::lock_guard<aios::hal::Mutex> lock(e.mu);
  ensure_searched(e);

  const Style st = style_of(face);
  const uint64_t key = (static_cast<uint64_t>(static_cast<int>(st)) << 40) |
                       (static_cast<uint64_t>(face.px) << 32) | cp;
  auto it = e.cache.find(key);
  if (it != e.cache.end()) return it->second.hit ? &it->second.r : nullptr;

  Cached entry;
  for (Ref& ref : e.slots[static_cast<int>(st)].chain) {
    FontFile* f = resolve(e, ref);
    if (f && cut(*f, cp, face, entry)) break;
  }
  if (!entry.hit) ++e.misses;
  e.cov_bytes += entry.cov.size();
  auto ins = e.cache.emplace(key, std::move(entry));
  Cached& held = ins.first->second;
  // The vector moved; its data pointer has to be taken again from where it
  // actually lives now, or every glyph past the first points at freed memory.
  if (held.hit && !held.cov.empty()) held.r.cov = held.cov.data();
  return held.hit ? &held.r : nullptr;
}

bool available() {
  Engine& e = engine();
  std::lock_guard<aios::hal::Mutex> lock(e.mu);
  ensure_searched(e);
  if (!e.fonts_held.empty()) return true;
  // Nothing read yet: open the first candidate, which is the one everything
  // Latin would come off anyway.
  for (Slot& s : e.slots)
    for (Ref& r : s.chain)
      if (resolve(e, r)) return true;
  return false;
}

bool install(const uint8_t* bytes, size_t len, const char* name) {
  if (!bytes || len < 12) return false;
  Engine& e = engine();
  std::lock_guard<aios::hal::Mutex> lock(e.mu);
  e.searched = true;  // an image that hands us bytes is not searching a disk
  std::vector<uint8_t> copy(bytes, bytes + len);
  FontFile* f = adopt(std::move(copy), name, 0);
  if (!f) return false;
  e.fonts_held.push_back(f);
  e.names += name ? name : "(embedded)";
  e.names += '\n';
  for (Slot& s : e.slots) {
    Ref r;
    r.path = name ? name : "(embedded)";
    r.f = f;
    r.tried = true;
    r.embedded = true;
    s.chain.push_back(r);
  }
  return true;
}

Stats stats() {
  Engine& e = engine();
  std::lock_guard<aios::hal::Mutex> lock(e.mu);
  return Stats{e.fonts_held.size(), e.cache.size(), e.misses, e.cov_bytes};
}

const char* installed_names() {
  Engine& e = engine();
  std::lock_guard<aios::hal::Mutex> lock(e.mu);
  ensure_searched(e);
  return e.names.c_str();
}

}  // namespace aios::userland::glyphs
