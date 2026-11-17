// Carving game assets out of pictures: a photo's subject cut from its background, its colours put on a game's
// palette, and its silhouette inflated into a closed, rounded mesh - natively, so a phone snapshot becomes a sprite or
// a model in the time of a request (apps/sqldoom/lib/carve.gr builds Doom's monsters from them).
//
//   image_cutout(canvas[, opts])  -> {canvas, mask, w, h, bbox: {x, y, w, h}, coverage, threshold, background}
//       The subject of a photo taken against a plainer background (paper, a wall, a table): the background's colours
//       are learnt from the picture's border (k-means in CIELAB), each pixel scored by how far it is from the nearest
//       of them, the threshold between the two found from the scores (Otsu's), the background grown in from the
//       border through what is below it, and what is left kept as the subject - its largest connected piece, with
//       its holes filled. The edge is then decided again from a colour model of the subject itself (each pixel near
//       the edge goes to whichever model it is closer to) and feathered over one pixel.
//       canvas: the picture with every pixel outside the subject made transparent; mask: bytes, one 0..255 a pixel
//       (top row first). opts: {tolerance (scales the threshold; 1), background: [r g b] (the background's colour,
//       instead of learning it), keep: "largest" | "all" (the pieces kept), max_side (the analysis size; 512)}.
//   image_quantize(canvas, palette[, opts]) -> {indices, mask, w, h}
//       Each pixel's nearest colour (CIELAB distance) among palette's (bytes, 3 a colour: R G B), with Floyd-Steinberg
//       error diffusion among the opaque pixels when opts.dither. indices: bytes, one a pixel; mask: bytes, 1 where
//       alpha >= opts.alpha (128). opts.exclude: palette indices never chosen.
//   image_resample(canvas, w, h[, crop]) -> canvas: the picture (or its crop: {x, y, w, h}) at w x h, each new pixel
//       the area-weighted mean of the old ones it covers (or the bilinear blend when enlarging), with alpha
//       premultiplied so a transparent neighbour's colour does not bleed into an edge.
//   mesh_inflate(mask, w, h[, opts]) -> {front, back, vertex_count, tris, width, height, depth}
//       A silhouette (mask: bytes, one a pixel, inside where >= 128) inflated into a closed surface: every point's
//       height over the picture plane is that of a circle across the silhouette there - the largest disc inside the
//       silhouette covering the point (the medial ball) is the cross-section's radius - so a limb is round and a body
//       is round. The rim is shared by the two halves; front faces +x, the picture's right is +y, up is +z (the "z"
//       up of r3d_model). Units are the mask's pixels, the rim's bottom at z = 0, the silhouette centred on y.
//       front, back: {positions, normals (float32 xyz a vertex), uvs (float32 uv: the vertex's place in the
//       picture, 0..1), indices (uint32), vertex_count} - model_prim's columns. opts: {thickness (the depth as a
//       share of the round cross-section; 1), smooth (passes smoothing the rim and the surface; 2)}.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "aios/userland/grain.hpp"
#include "grain_builtins_internal.hpp"

namespace aios::userland::grain {

namespace {

MapObj* opts_map(std::vector<Value>& args, size_t i) {
  return args.size() > i && args[i].type == Type::Map ? static_cast<MapObj*>(args[i].obj.get()) : nullptr;
}
double opt_num(MapObj* m, const char* k, double dflt) {
  if (!m) return dflt;
  const Value* v = m->find(k);
  return v && v->is_number() ? v->as_double() : dflt;
}
bool opt_bool(MapObj* m, const char* k, bool dflt) {
  if (!m) return dflt;
  const Value* v = m->find(k);
  return v ? v->truthy() : dflt;
}
const std::vector<uint8_t>* bytes_arg(const Value& v) {
  return v.type == Type::Bytes ? &v.bytes_data() : nullptr;
}
std::vector<uint8_t> bytes_of(const Value& v) {
  if (v.type == Type::Bytes) return v.bytes_data();
  if (v.type == Type::Str) return std::vector<uint8_t>(v.str().begin(), v.str().end());
  return {};
}

// ---- colour ---------------------------------------------------------------------------------------------------

struct Lab { float l, a, b; };

float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

struct LabTable {
  float lin[256];
  LabTable() { for (int i = 0; i < 256; ++i) lin[i] = srgb_to_linear(i / 255.0f); }
};
const LabTable& lab_table() { static LabTable t; return t; }

float lab_f(float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.0f / 116.0f; }

Lab to_lab(uint8_t r8, uint8_t g8, uint8_t b8) {
  const auto& T = lab_table();
  const float r = T.lin[r8], g = T.lin[g8], b = T.lin[b8];
  const float x = (0.4124f * r + 0.3576f * g + 0.1805f * b) / 0.95047f;
  const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
  const float z = (0.0193f * r + 0.1192f * g + 0.9505f * b) / 1.08883f;
  const float fx = lab_f(x), fy = lab_f(y), fz = lab_f(z);
  return {116.0f * fy - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz)};
}
Lab to_lab(uint32_t argb) { return to_lab((uint8_t)(argb >> 16), (uint8_t)(argb >> 8), (uint8_t)argb); }

float dist2(const Lab& p, const Lab& q) {
  const float dl = p.l - q.l, da = p.a - q.a, db = p.b - q.b;
  return dl * dl + da * da + db * db;
}

// k-means over samples (deterministic: the farthest-point start, then Lloyd's), returning the centres and each
// one's spread (the root mean square distance of its members).
struct Cluster { Lab c; float spread; size_t n; };
std::vector<Cluster> kmeans(const std::vector<Lab>& s, int k) {
  std::vector<Cluster> out;
  if (s.empty()) return out;
  k = std::min<int>(k, (int)s.size());
  std::vector<Lab> c;
  // start from the mean's nearest sample, then each next centre is the sample farthest from those chosen
  Lab mean{0, 0, 0};
  for (const Lab& p : s) { mean.l += p.l; mean.a += p.a; mean.b += p.b; }
  mean.l /= s.size(); mean.a /= s.size(); mean.b /= s.size();
  size_t best = 0;
  float bd = std::numeric_limits<float>::max();
  for (size_t i = 0; i < s.size(); ++i) { float d = dist2(s[i], mean); if (d < bd) { bd = d; best = i; } }
  c.push_back(s[best]);
  std::vector<float> nd(s.size(), std::numeric_limits<float>::max());
  while ((int)c.size() < k) {
    size_t far = 0;
    float fd = -1;
    for (size_t i = 0; i < s.size(); ++i) {
      nd[i] = std::min(nd[i], dist2(s[i], c.back()));
      if (nd[i] > fd) { fd = nd[i]; far = i; }
    }
    if (fd <= 1.0f) break;   // the rest are within one unit of a centre: no more clusters to find
    c.push_back(s[far]);
  }
  std::vector<int> as(s.size(), 0);
  for (int it = 0; it < 12; ++it) {
    std::vector<double> sl(c.size(), 0), sa(c.size(), 0), sb(c.size(), 0);
    std::vector<size_t> n(c.size(), 0);
    for (size_t i = 0; i < s.size(); ++i) {
      int bi = 0;
      float b2 = std::numeric_limits<float>::max();
      for (size_t j = 0; j < c.size(); ++j) { float d = dist2(s[i], c[j]); if (d < b2) { b2 = d; bi = (int)j; } }
      as[i] = bi;
      sl[bi] += s[i].l; sa[bi] += s[i].a; sb[bi] += s[i].b; ++n[bi];
    }
    for (size_t j = 0; j < c.size(); ++j)
      if (n[j]) c[j] = {(float)(sl[j] / n[j]), (float)(sa[j] / n[j]), (float)(sb[j] / n[j])};
  }
  std::vector<double> sp(c.size(), 0);
  std::vector<size_t> n(c.size(), 0);
  for (size_t i = 0; i < s.size(); ++i) { sp[as[i]] += dist2(s[i], c[as[i]]); ++n[as[i]]; }
  for (size_t j = 0; j < c.size(); ++j)
    if (n[j]) out.push_back({c[j], (float)std::sqrt(sp[j] / n[j]), n[j]});
  return out;
}

// how far p is from a set of clusters, in units of each cluster's spread (with a floor, so a flat background does
// not make every small difference look large)
float cluster_distance(const Lab& p, const std::vector<Cluster>& cs, float floor) {
  float best = std::numeric_limits<float>::max();
  for (const Cluster& c : cs) best = std::min(best, std::sqrt(dist2(p, c.c)) / std::max(c.spread, floor));
  return best;
}

// ---- masks ----------------------------------------------------------------------------------------------------

// connected pieces (4-neighbours) of the cells where m is set: each cell's piece number (-1 outside), and sizes
int label_pieces(const std::vector<uint8_t>& m, int w, int h, std::vector<int>& lab, std::vector<size_t>& sizes) {
  lab.assign(m.size(), -1);
  sizes.clear();
  std::vector<int> stack;
  for (int i = 0; i < w * h; ++i) {
    if (!m[i] || lab[i] >= 0) continue;
    const int id = (int)sizes.size();
    size_t n = 0;
    stack.push_back(i);
    lab[i] = id;
    while (!stack.empty()) {
      const int p = stack.back();
      stack.pop_back();
      ++n;
      const int x = p % w, y = p / w;
      const int nb[4] = {x > 0 ? p - 1 : -1, x < w - 1 ? p + 1 : -1, y > 0 ? p - w : -1, y < h - 1 ? p + w : -1};
      for (int q : nb)
        if (q >= 0 && m[q] && lab[q] < 0) { lab[q] = id; stack.push_back(q); }
    }
    sizes.push_back(n);
  }
  return (int)sizes.size();
}

// what of the outside cannot be reached from the picture's border is a hole: filled
void fill_holes(std::vector<uint8_t>& m, int w, int h) {
  std::vector<uint8_t> reach(m.size(), 0);
  std::vector<int> stack;
  auto seed = [&](int p) { if (!m[p] && !reach[p]) { reach[p] = 1; stack.push_back(p); } };
  for (int x = 0; x < w; ++x) { seed(x); seed((h - 1) * w + x); }
  for (int y = 0; y < h; ++y) { seed(y * w); seed(y * w + w - 1); }
  while (!stack.empty()) {
    const int p = stack.back();
    stack.pop_back();
    const int x = p % w, y = p / w;
    if (x > 0) seed(p - 1);
    if (x < w - 1) seed(p + 1);
    if (y > 0) seed(p - w);
    if (y < h - 1) seed(p + w);
  }
  for (size_t i = 0; i < m.size(); ++i) if (!m[i] && !reach[i]) m[i] = 1;
}

// erosion / dilation by a square of radius r (separable: a run's minimum / maximum, along rows then columns)
void morph(std::vector<uint8_t>& m, int w, int h, int r, bool dilate) {
  if (r <= 0) return;
  std::vector<uint8_t> t(m.size());
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      uint8_t v = dilate ? 0 : 1;
      for (int k = std::max(0, x - r); k <= std::min(w - 1, x + r); ++k) v = dilate ? (uint8_t)(v | m[y * w + k]) : (uint8_t)(v & m[y * w + k]);
      t[y * w + x] = v;
    }
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      uint8_t v = dilate ? 0 : 1;
      for (int k = std::max(0, y - r); k <= std::min(h - 1, y + r); ++k) v = dilate ? (uint8_t)(v | t[k * w + x]) : (uint8_t)(v & t[k * w + x]);
      m[y * w + x] = v;
    }
}

// The exact Euclidean distance transform (Felzenszwalb and Huttenlocher): each cell's distance to the nearest cell
// where `inside` is false (cells outside are 0). Squared distances along columns, then the lower envelope of
// parabolas along rows.
std::vector<float> distance_inside(const std::vector<uint8_t>& inside, int w, int h) {
  const double BIG = 1e20;   // "no outside cell in this line" (finite: the envelope's arithmetic stays defined)
  const double INF = std::numeric_limits<double>::infinity();
  std::vector<double> f((size_t)w * h);
  for (size_t i = 0; i < f.size(); ++i) f[i] = inside[i] ? BIG : 0.0;
  auto pass = [&](double* d, int n, int stride) {
    std::vector<double> g(n), z(n + 1);
    std::vector<int> v(n);
    for (int i = 0; i < n; ++i) g[i] = d[(size_t)i * stride];
    int k = 0;
    v[0] = 0;
    z[0] = -INF;
    z[1] = INF;
    auto meet = [&](int q, int p) { return ((g[q] + (double)q * q) - (g[p] + (double)p * p)) / (2.0 * q - 2.0 * p); };
    for (int q = 1; q < n; ++q) {
      double s = meet(q, v[k]);
      while (s <= z[k]) { --k; s = meet(q, v[k]); }
      ++k;
      v[k] = q;
      z[k] = s;
      z[k + 1] = INF;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
      while (z[k + 1] < q) ++k;
      const double dq = (double)(q - v[k]);
      d[(size_t)q * stride] = dq * dq + g[v[k]];
    }
  };
  for (int x = 0; x < w; ++x) pass(&f[x], h, w);
  for (int y = 0; y < h; ++y) pass(&f[(size_t)y * w], w, 1);
  // (the picture's own edge counts as outside: a subject running off it still closes)
  std::vector<float> out((size_t)w * h, 0.0f);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const size_t i = (size_t)y * w + x;
      if (!inside[i]) continue;
      const double e = (double)std::min(std::min(x + 1, w - x), std::min(y + 1, h - y));
      out[i] = (float)std::min(std::sqrt(f[i]), e);
    }
  return out;
}

// ---- image_cutout ---------------------------------------------------------------------------------------------

Value bi_image_cutout(Vm&, std::vector<Value>& args, std::string& error) {
  uint32_t W = 0, H = 0;
  const std::vector<uint32_t>* px = nullptr;
  if (!canvas_pixels(args[0], &W, &H, &px, error, "image_cutout")) return Value::nil();
  if (W < 8 || H < 8) { error = "image_cutout(): the picture is smaller than 8x8"; return Value::nil(); }
  MapObj* o = opts_map(args, 1);
  const double tolerance = std::max(0.05, opt_num(o, "tolerance", 1.0));
  const int max_side = (int)std::clamp(opt_num(o, "max_side", 512), 64.0, 2048.0);
  bool keep_all = false;
  if (o) if (const Value* k = o->find("keep"); k && k->type == Type::Str) {
    if (k->str() == "all") keep_all = true;
    else if (k->str() != "largest") { error = "image_cutout(): keep is \"largest\" or \"all\", not \"" + k->str() + "\""; return Value::nil(); }
  }

  // the analysis picture: box-filtered down to max_side, each cell in CIELAB
  const double sc = std::max(1.0, (double)std::max(W, H) / max_side);
  const int w = std::max(8, (int)std::lround(W / sc)), h = std::max(8, (int)std::lround(H / sc));
  std::vector<Lab> L((size_t)w * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const int x0 = (int)(x * (double)W / w), x1 = std::max(x0 + 1, (int)((x + 1) * (double)W / w));
      const int y0 = (int)(y * (double)H / h), y1 = std::max(y0 + 1, (int)((y + 1) * (double)H / h));
      double r = 0, g = 0, b = 0;
      int n = 0;
      for (int yy = y0; yy < std::min<int>(y1, H); ++yy)
        for (int xx = x0; xx < std::min<int>(x1, W); ++xx) {
          const uint32_t p = (*px)[(size_t)yy * W + xx];
          r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255; ++n;
        }
      L[(size_t)y * w + x] = to_lab((uint8_t)(r / n + 0.5), (uint8_t)(g / n + 0.5), (uint8_t)(b / n + 0.5));
    }

  // the background's colours: given, or learnt from a band around the border
  std::vector<Cluster> bg;
  Lab given{0, 0, 0};
  bool have_given = false;
  if (o) if (const Value* bv = o->find("background"); bv && bv->type == Type::List && bv->list_items().size() >= 3) {
    const auto& it = bv->list_items();
    given = to_lab((uint8_t)std::clamp<int64_t>(it[0].as_int(), 0, 255), (uint8_t)std::clamp<int64_t>(it[1].as_int(), 0, 255),
                   (uint8_t)std::clamp<int64_t>(it[2].as_int(), 0, 255));
    have_given = true;
  }
  const int band = std::max(2, std::min(w, h) / 40);
  std::vector<Lab> border;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      if (x < band || y < band || x >= w - band || y >= h - band) border.push_back(L[(size_t)y * w + x]);
  if (have_given) {
    float sp = 0;
    for (const Lab& p : border) sp += dist2(p, given);
    bg.push_back({given, std::sqrt(sp / border.size()), border.size()});
  } else {
    bg = kmeans(border, 5);
    // a cluster holding under 3% of the border is the subject touching it, not background
    std::vector<Cluster> kept;
    for (const Cluster& c : bg) if (c.n * 100 >= border.size() * 3) kept.push_back(c);
    if (!kept.empty()) bg = std::move(kept);
  }
  const float floor = 2.5f;   // ΔE: a background's spread is never taken as less than this
  std::vector<float> score((size_t)w * h);
  float smax = 0;
  for (size_t i = 0; i < score.size(); ++i) { score[i] = cluster_distance(L[i], bg, floor); smax = std::max(smax, score[i]); }

  // Otsu's threshold over the scores
  const int NB = 256;
  std::vector<double> hist(NB, 0);
  const float top = std::max(smax, 1e-3f);
  for (float s : score) hist[std::min(NB - 1, (int)(s / top * (NB - 1)))] += 1;
  double total = (double)score.size(), sum = 0;
  for (int i = 0; i < NB; ++i) sum += i * hist[i];
  double wb = 0, sb = 0, best_var = -1;
  int best_t = 0;
  for (int t = 0; t < NB; ++t) {
    wb += hist[t];
    if (wb == 0) continue;
    const double wf = total - wb;
    if (wf == 0) break;
    sb += t * hist[t];
    const double mb = sb / wb, mf = (sum - sb) / wf;
    const double var = wb * wf * (mb - mf) * (mb - mf);
    if (var > best_var) { best_var = var; best_t = t; }
  }
  // never closer to the background than 3 of its spreads: a picture that is nearly all background still has one
  float thr = std::max(3.0f, (float)((best_t + 0.5) / (NB - 1) * top));
  thr = (float)(thr * tolerance);

  // the background: grown in from the border through cells scoring under the threshold
  std::vector<uint8_t> fg((size_t)w * h, 1);
  {
    std::vector<int> stack;
    auto seed = [&](int p) { if (fg[p] && score[p] < thr) { fg[p] = 0; stack.push_back(p); } };
    for (int x = 0; x < w; ++x) { seed(x); seed((h - 1) * w + x); }
    for (int y = 0; y < h; ++y) { seed(y * w); seed(y * w + w - 1); }
    while (!stack.empty()) {
      const int p = stack.back();
      stack.pop_back();
      const int x = p % w, y = p / w;
      if (x > 0) seed(p - 1);
      if (x < w - 1) seed(p + 1);
      if (y > 0) seed(p - w);
      if (y < h - 1) seed(p + w);
    }
  }
  // specks off, the subject's piece(s) kept, holes filled
  const int open_r = std::max(1, std::min(w, h) / 256);
  auto keep_pieces = [&](std::vector<uint8_t>& m) {
    std::vector<int> lab;
    std::vector<size_t> sizes;
    const int n = label_pieces(m, w, h, lab, sizes);
    if (n == 0) return;
    const size_t biggest = *std::max_element(sizes.begin(), sizes.end());
    for (size_t i = 0; i < m.size(); ++i) {
      if (lab[i] < 0) continue;
      const size_t s = sizes[(size_t)lab[i]];
      m[i] = keep_all ? (s * 50 >= biggest ? 1 : 0) : (s == biggest ? 1 : 0);
    }
  };
  morph(fg, w, h, open_r, false);
  morph(fg, w, h, open_r, true);
  keep_pieces(fg);
  fill_holes(fg, w, h);
  size_t nfg = 0;
  for (uint8_t v : fg) nfg += v;
  if (nfg == 0) { error = "image_cutout(): nothing stands out from the background (try a plainer background, or opts.tolerance under 1)"; return Value::nil(); }

  // the edge decided again: a colour model of the subject from its cells away from the edge, and every cell within a
  // band of the edge given to the nearer model
  {
    std::vector<uint8_t> core = fg;
    const int br = std::max(2, std::min(w, h) / 64);
    morph(core, w, h, br, false);
    std::vector<Lab> fs;
    for (size_t i = 0; i < core.size(); ++i) if (core[i]) fs.push_back(L[i]);
    if (fs.size() >= 64) {
      // a sample is enough for the model
      std::vector<Lab> sample;
      const size_t step = std::max<size_t>(1, fs.size() / 20000);
      for (size_t i = 0; i < fs.size(); i += step) sample.push_back(fs[i]);
      const std::vector<Cluster> fgm = kmeans(sample, 6);
      std::vector<uint8_t> grow = fg, shrink = fg;
      morph(grow, w, h, br, true);
      morph(shrink, w, h, br, false);
      for (size_t i = 0; i < fg.size(); ++i) {
        if (grow[i] == shrink[i]) continue;   // not near the edge
        const float db = cluster_distance(L[i], bg, floor), df = cluster_distance(L[i], fgm, floor);
        fg[i] = (df < db && db >= thr * 0.5f) ? 1 : 0;
      }
      keep_pieces(fg);
      fill_holes(fg, w, h);
    }
  }

  // the full-size mask: the analysis mask sampled bilinearly (a one-pixel feather at the analysis size)
  std::vector<uint8_t> mask((size_t)W * H);
  std::vector<uint32_t> outpx(*px);
  uint32_t bx0 = W, by0 = H, bx1 = 0, by1 = 0;
  size_t cover = 0;
  for (uint32_t y = 0; y < H; ++y) {
    const double fy = std::clamp((y + 0.5) / sc - 0.5, 0.0, (double)h - 1);
    const int y0 = (int)fy, y1 = std::min(h - 1, y0 + 1);
    const double ty = fy - y0;
    for (uint32_t x = 0; x < W; ++x) {
      const double fx = std::clamp((x + 0.5) / sc - 0.5, 0.0, (double)w - 1);
      const int x0 = (int)fx, x1 = std::min(w - 1, x0 + 1);
      const double tx = fx - x0;
      const double v = (fg[(size_t)y0 * w + x0] * (1 - tx) + fg[(size_t)y0 * w + x1] * tx) * (1 - ty) +
                       (fg[(size_t)y1 * w + x0] * (1 - tx) + fg[(size_t)y1 * w + x1] * tx) * ty;
      // sharpened around one half, so the feather is about a pixel at full size
      const double a = std::clamp((v - 0.5) * std::max(2.0, sc) + 0.5, 0.0, 1.0);
      const uint8_t a8 = (uint8_t)std::lround(a * 255);
      const size_t i = (size_t)y * W + x;
      mask[i] = a8;
      const uint32_t p = outpx[i];
      const uint32_t pa = (p >> 24) * a8 / 255;
      outpx[i] = (pa << 24) | (p & 0xFFFFFF);
      if (a8 >= 128) {
        ++cover;
        bx0 = std::min(bx0, x); by0 = std::min(by0, y); bx1 = std::max(bx1, x); by1 = std::max(by1, y);
      }
    }
  }
  if (cover == 0) { error = "image_cutout(): the subject vanished at full size"; return Value::nil(); }
  auto bbox = std::make_shared<MapObj>();
  bbox->put("x", Value::integer(bx0));
  bbox->put("y", Value::integer(by0));
  bbox->put("w", Value::integer(bx1 - bx0 + 1));
  bbox->put("h", Value::integer(by1 - by0 + 1));
  // the background's main colour, back in sRGB-ish terms: the mean of the border cells the cut left outside
  double br_ = 0, bg_ = 0, bb_ = 0;
  size_t nb = 0;
  for (uint32_t y = 0; y < H; y += std::max<uint32_t>(1, H / 64))
    for (uint32_t x = 0; x < W; x += std::max<uint32_t>(1, W / 64)) {
      const size_t i = (size_t)y * W + x;
      if (mask[i] >= 128) continue;
      const uint32_t p = (*px)[i];
      br_ += (p >> 16) & 255; bg_ += (p >> 8) & 255; bb_ += p & 255; ++nb;
    }
  std::vector<Value> bgc;
  if (nb) bgc = {Value::integer(std::lround(br_ / nb)), Value::integer(std::lround(bg_ / nb)), Value::integer(std::lround(bb_ / nb))};
  auto res = std::make_shared<MapObj>();
  res->put("canvas", canvas_value(W, H, std::move(outpx)));
  res->put("mask", Value::bytes(std::move(mask)));
  res->put("w", Value::integer(W));
  res->put("h", Value::integer(H));
  res->put("bbox", Value::object(Type::Map, bbox));
  res->put("coverage", Value::real((double)cover / ((double)W * H)));
  res->put("threshold", Value::real(thr));
  res->put("background", Value::list(std::move(bgc)));
  return Value::object(Type::Map, res);
}

// ---- image_quantize -------------------------------------------------------------------------------------------

Value bi_image_quantize(Vm&, std::vector<Value>& args, std::string& error) {
  uint32_t W = 0, H = 0;
  const std::vector<uint32_t>* px = nullptr;
  if (!canvas_pixels(args[0], &W, &H, &px, error, "image_quantize")) return Value::nil();
  const std::vector<uint8_t> pal = bytes_of(args[1]);
  if (pal.size() < 3 || pal.size() % 3 != 0 || pal.size() > 768) { error = "image_quantize(): the palette wants bytes, 3 a colour (R G B), 1 to 256 colours"; return Value::nil(); }
  MapObj* o = opts_map(args, 2);
  const bool dither = opt_bool(o, "dither", false);
  const int alpha_cut = (int)std::clamp(opt_num(o, "alpha", 128), 1.0, 255.0);
  const int ncol = (int)(pal.size() / 3);
  std::vector<uint8_t> allowed((size_t)ncol, 1);
  if (o) if (const Value* ex = o->find("exclude"); ex && ex->type == Type::List)
    for (const Value& v : ex->list_items()) if (v.is_number() && v.as_int() >= 0 && v.as_int() < ncol) allowed[(size_t)v.as_int()] = 0;
  std::vector<Lab> pl((size_t)ncol);
  bool any = false;
  for (int i = 0; i < ncol; ++i) { pl[i] = to_lab(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]); any = any || allowed[i]; }
  if (!any) { error = "image_quantize(): every palette colour is excluded"; return Value::nil(); }
  // exact (no binning of the colours), memoised by the colour
  std::unordered_map<uint32_t, int> memo;
  auto nearest = [&](int r, int g, int b) -> int {
    const uint32_t key = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    if (auto it = memo.find(key); it != memo.end()) return it->second;
    const Lab p = to_lab((uint8_t)r, (uint8_t)g, (uint8_t)b);
    int bi = 0;
    float bd = std::numeric_limits<float>::max();
    for (int i = 0; i < ncol; ++i) {
      if (!allowed[i]) continue;
      const float d = dist2(p, pl[i]);
      if (d < bd) { bd = d; bi = i; }
    }
    memo[key] = bi;
    return bi;
  };
  std::vector<uint8_t> idx((size_t)W * H, 0), mask((size_t)W * H, 0);
  // error carried in sRGB units, on this row and the next
  std::vector<float> e0((size_t)(W + 2) * 3, 0), e1((size_t)(W + 2) * 3, 0);
  for (uint32_t y = 0; y < H; ++y) {
    std::fill(e1.begin(), e1.end(), 0.0f);
    for (uint32_t x = 0; x < W; ++x) {
      const size_t i = (size_t)y * W + x;
      const uint32_t p = (*px)[i];
      if ((int)(p >> 24) < alpha_cut) continue;
      mask[i] = 1;
      float c[3] = {(float)((p >> 16) & 255), (float)((p >> 8) & 255), (float)(p & 255)};
      if (dither) for (int k = 0; k < 3; ++k) c[k] = std::clamp(c[k] + e0[(x + 1) * 3 + k], 0.0f, 255.0f);
      const int q = nearest((int)std::lround(c[0]), (int)std::lround(c[1]), (int)std::lround(c[2]));
      idx[i] = (uint8_t)q;
      if (!dither) continue;
      for (int k = 0; k < 3; ++k) {
        // only toward opaque neighbours: the error of the subject's edge is not pushed into nothing
        const float err = c[k] - pal[(size_t)q * 3 + k];
        const bool r_op = x + 1 < W && (int)((*px)[i + 1] >> 24) >= alpha_cut;
        const bool d_op = y + 1 < H && (int)((*px)[i + W] >> 24) >= alpha_cut;
        if (r_op) e0[(x + 2) * 3 + k] += err * 7 / 16;
        if (d_op) {
          if (x > 0) e1[x * 3 + k] += err * 3 / 16;
          e1[(x + 1) * 3 + k] += err * 5 / 16;
          if (x + 1 < W) e1[(x + 2) * 3 + k] += err * 1 / 16;
        }
      }
    }
    std::swap(e0, e1);
  }
  auto res = std::make_shared<MapObj>();
  res->put("indices", Value::bytes(std::move(idx)));
  res->put("mask", Value::bytes(std::move(mask)));
  res->put("w", Value::integer(W));
  res->put("h", Value::integer(H));
  return Value::object(Type::Map, res);
}

// ---- image_resample -------------------------------------------------------------------------------------------

Value bi_image_resample(Vm&, std::vector<Value>& args, std::string& error) {
  uint32_t W = 0, H = 0;
  const std::vector<uint32_t>* px = nullptr;
  if (!canvas_pixels(args[0], &W, &H, &px, error, "image_resample")) return Value::nil();
  if (!want_number(args[1], error, "image_resample") || !want_number(args[2], error, "image_resample")) return Value::nil();
  const int64_t ow = args[1].as_int(), oh = args[2].as_int();
  if (ow < 1 || oh < 1 || ow > 16384 || oh > 16384) { error = "image_resample(): the new size is 1..16384 each way"; return Value::nil(); }
  double cx = 0, cy = 0, cw = W, ch = H;
  if (MapObj* c = opts_map(args, 3)) {
    cx = opt_num(c, "x", 0); cy = opt_num(c, "y", 0); cw = opt_num(c, "w", W); ch = opt_num(c, "h", H);
    if (cw <= 0 || ch <= 0 || cx < 0 || cy < 0 || cx + cw > W + 1e-9 || cy + ch > H + 1e-9) {
      error = "image_resample(): the crop is not inside the picture";
      return Value::nil();
    }
  }
  // premultiplied channels, as doubles
  auto at = [&](int x, int y, double* o) {
    const uint32_t p = (*px)[(size_t)y * W + x];
    const double a = (p >> 24) / 255.0;
    o[0] += ((p >> 16) & 255) * a; o[1] += ((p >> 8) & 255) * a; o[2] += (p & 255) * a; o[3] += a;
  };
  std::vector<uint32_t> out((size_t)ow * oh);
  const double fx = cw / ow, fy = ch / oh;
  for (int64_t y = 0; y < oh; ++y)
    for (int64_t x = 0; x < ow; ++x) {
      double acc[4] = {0, 0, 0, 0}, wsum = 0;
      if (fx > 1.0 || fy > 1.0) {   // shrinking: the covered area, each old pixel by how much of it is covered
        const double x0 = cx + x * fx, x1 = x0 + fx, y0 = cy + y * fy, y1 = y0 + fy;
        for (int yy = (int)y0; yy < std::min<int>((int)std::ceil(y1), H); ++yy) {
          const double wy = std::min<double>(yy + 1, y1) - std::max<double>(yy, y0);
          if (wy <= 0) continue;
          for (int xx = (int)x0; xx < std::min<int>((int)std::ceil(x1), W); ++xx) {
            const double wx = std::min<double>(xx + 1, x1) - std::max<double>(xx, x0);
            if (wx <= 0) continue;
            double o[4] = {0, 0, 0, 0};
            at(xx, yy, o);
            for (int k = 0; k < 4; ++k) acc[k] += o[k] * wx * wy;
            wsum += wx * wy;
          }
        }
      } else {   // enlarging: bilinear
        const double sx = std::clamp(cx + (x + 0.5) * fx - 0.5, 0.0, (double)W - 1), sy = std::clamp(cy + (y + 0.5) * fy - 0.5, 0.0, (double)H - 1);
        const int x0 = (int)sx, y0 = (int)sy, x1 = std::min<int>(x0 + 1, W - 1), y1 = std::min<int>(y0 + 1, H - 1);
        const double tx = sx - x0, ty = sy - y0;
        const int xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
        const double ws[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
        for (int q = 0; q < 4; ++q) {
          double o[4] = {0, 0, 0, 0};
          at(xs[q], ys[q], o);
          for (int k = 0; k < 4; ++k) acc[k] += o[k] * ws[q];
          wsum += ws[q];
        }
      }
      if (wsum <= 0) continue;
      const double a = acc[3] / wsum;
      uint32_t r = 0, g = 0, b = 0;
      if (acc[3] > 0) {
        r = (uint32_t)std::clamp(std::lround(acc[0] / acc[3]), 0L, 255L);
        g = (uint32_t)std::clamp(std::lround(acc[1] / acc[3]), 0L, 255L);
        b = (uint32_t)std::clamp(std::lround(acc[2] / acc[3]), 0L, 255L);
      }
      out[(size_t)y * ow + x] = ((uint32_t)std::lround(a * 255) << 24) | (r << 16) | (g << 8) | b;
    }
  return canvas_value((uint32_t)ow, (uint32_t)oh, std::move(out));
}

// ---- mesh_inflate ---------------------------------------------------------------------------------------------

void put_f32(std::vector<uint8_t>& b, float f) { uint8_t t[4]; std::memcpy(t, &f, 4); b.insert(b.end(), t, t + 4); }
void put_u32(std::vector<uint8_t>& b, uint32_t u) { b.push_back((uint8_t)u); b.push_back((uint8_t)(u >> 8)); b.push_back((uint8_t)(u >> 16)); b.push_back((uint8_t)(u >> 24)); }

Value bi_mesh_inflate(Vm&, std::vector<Value>& args, std::string& error) {
  const std::vector<uint8_t>* mb = bytes_arg(args[0]);
  if (!mb) { error = std::string("mesh_inflate() wants the mask as bytes, got a ") + type_name(args[0].type); return Value::nil(); }
  if (!want_number(args[1], error, "mesh_inflate") || !want_number(args[2], error, "mesh_inflate")) return Value::nil();
  const int w = (int)args[1].as_int(), h = (int)args[2].as_int();
  if (w < 3 || h < 3 || w > 4096 || h > 4096 || (size_t)w * h != mb->size()) {
    error = "mesh_inflate(): the mask is " + std::to_string(mb->size()) + " bytes, not w*h (" + std::to_string(w) + "x" + std::to_string(h) + ", each 3..4096)";
    return Value::nil();
  }
  MapObj* o = opts_map(args, 3);
  const double thickness = std::clamp(opt_num(o, "thickness", 1.0), 0.01, 4.0);
  const int smooth = (int)std::clamp(opt_num(o, "smooth", 2), 0.0, 20.0);
  std::vector<uint8_t> in((size_t)w * h);
  size_t nin = 0;
  for (size_t i = 0; i < in.size(); ++i) { in[i] = (*mb)[i] >= 128; nin += in[i]; }
  if (nin < 4) { error = "mesh_inflate(): the mask has nothing inside"; return Value::nil(); }

  const std::vector<float> D = distance_inside(in, w, h);
  // the medial balls: a cell's ball is maximal when no neighbour's ball contains it (D(q) >= D(p) + |p - q|)
  struct Ball { int x, y; float r; };
  std::vector<Ball> balls;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float d = D[(size_t)y * w + x];
      if (d <= 0) continue;
      bool maximal = true;
      for (int dy = -1; dy <= 1 && maximal; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          if (!dx && !dy) continue;
          const int nx = x + dx, ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
          if (D[(size_t)ny * w + nx] >= d + std::sqrt((float)(dx * dx + dy * dy)) - 0.02f) { maximal = false; break; }
        }
      if (maximal) balls.push_back({x, y, d});
    }
  // each cell's cross-section radius: the largest medial ball covering it (painted biggest first)
  std::sort(balls.begin(), balls.end(), [](const Ball& a, const Ball& b) { return a.r > b.r; });
  std::vector<float> R((size_t)w * h, 0.0f);
  for (const Ball& b : balls) {
    const int r = (int)std::ceil(b.r);
    const float r2 = b.r * b.r;
    for (int y = std::max(0, b.y - r); y <= std::min(h - 1, b.y + r); ++y)
      for (int x = std::max(0, b.x - r); x <= std::min(w - 1, b.x + r); ++x) {
        const size_t i = (size_t)y * w + x;
        if (!in[i] || R[i] > 0) continue;
        const float dx = (float)(x - b.x), dy = (float)(y - b.y);
        if (dx * dx + dy * dy <= r2) R[i] = b.r;
      }
  }
  // the height: the circle of radius R across the cell's distance from the rim (the rim sits half a cell out)
  std::vector<float> Z((size_t)w * h, 0.0f);
  for (size_t i = 0; i < Z.size(); ++i) {
    if (!in[i]) continue;
    const float r = std::max(R[i], D[i]);
    const float d = std::min(r, std::max(0.0f, D[i] - 0.5f));
    Z[i] = std::sqrt(std::max(0.0f, 2.0f * r * d - d * d));
  }

  // vertices at the cells' centres: the rim (inside cells with an outside 4-neighbour, or on the picture's edge) is
  // one vertex shared by both halves at depth 0; every other inside cell is a vertex on each half
  auto inside = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && in[(size_t)y * w + x]; };
  std::vector<uint8_t> rim((size_t)w * h, 0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      if (inside(x, y) && (!inside(x - 1, y) || !inside(x + 1, y) || !inside(x, y - 1) || !inside(x, y + 1))) rim[(size_t)y * w + x] = 1;
  // the surface smoothed (the medial ridges leave creases): each inner cell's height averaged with its neighbours'
  for (int s = 0; s < smooth * 2; ++s) {
    std::vector<float> Z2 = Z;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const size_t i = (size_t)y * w + x;
        if (!in[i] || rim[i]) continue;
        float sum = 0;
        int n = 0;
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx)
            if (inside(x + dx, y + dy)) { sum += Z[(size_t)(y + dy) * w + x + dx]; ++n; }
        Z2[i] = sum / n;
      }
    Z = std::move(Z2);
  }
  for (size_t i = 0; i < Z.size(); ++i) if (rim[i]) Z[i] = 0;

  // vertex numbering: front (rim included), then back (inner cells only)
  std::vector<int> vf((size_t)w * h, -1), vb((size_t)w * h, -1);
  struct V { float x, y, z, u, v; };
  std::vector<V> verts;
  const float cx = w * 0.5f;
  int ymax = 0;
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) if (in[(size_t)y * w + x]) ymax = std::max(ymax, y);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const size_t i = (size_t)y * w + x;
      if (!in[i]) continue;
      vf[i] = (int)verts.size();
      verts.push_back({(float)x + 0.5f - cx, (float)(ymax - y), (float)(Z[i] * thickness), (x + 0.5f) / w, (y + 0.5f) / h});
    }
  const size_t nfront = verts.size();
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const size_t i = (size_t)y * w + x;
      if (!in[i] || rim[i]) { vb[i] = vf[i]; continue; }
      vb[i] = (int)verts.size();
      verts.push_back({verts[(size_t)vf[i]].x, verts[(size_t)vf[i]].y, -verts[(size_t)vf[i]].z, verts[(size_t)vf[i]].u, verts[(size_t)vf[i]].v});
    }
  // the rim smoothed in the picture's plane (the cells' staircase): each rim vertex toward its rim neighbours' mean
  for (int s = 0; s < smooth; ++s) {
    std::vector<V> nv = verts;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const size_t i = (size_t)y * w + x;
        if (!rim[i]) continue;
        float sx = 0, sy = 0;
        int n = 0;
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx) {
            if (!dx && !dy) continue;
            const int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h || !rim[(size_t)ny * w + nx]) continue;
            sx += verts[(size_t)vf[(size_t)ny * w + nx]].x;
            sy += verts[(size_t)vf[(size_t)ny * w + nx]].y;
            ++n;
          }
        if (n < 2) continue;
        V& t = nv[(size_t)vf[i]];
        t.x = 0.5f * t.x + 0.5f * sx / n;
        t.y = 0.5f * t.y + 0.5f * sy / n;
      }
    verts = std::move(nv);
  }

  // triangles: each square of four cell centres with all four inside is two, one with three inside is one; the
  // front wound counter-clockwise seen from +z, the back the other way
  std::vector<uint32_t> tf, tb;
  auto tri = [&](int a, int b, int c) {   // a b c: cells, counter-clockwise in the picture seen from the front
    tf.insert(tf.end(), {(uint32_t)vf[a], (uint32_t)vf[b], (uint32_t)vf[c]});
    tb.insert(tb.end(), {(uint32_t)vb[a], (uint32_t)vb[c], (uint32_t)vb[b]});
  };
  for (int y = 0; y + 1 < h; ++y)
    for (int x = 0; x + 1 < w; ++x) {
      // (the picture's y runs down, the mesh's up: counter-clockwise on screen is clockwise in rows)
      const int a = y * w + x, b = a + 1, c = a + w, d = c + 1;   // a b / c d
      const bool ia = in[a], ib = in[b], ic = in[c], id = in[d];
      const int n = ia + ib + ic + id;
      if (n == 4) { tri(a, c, d); tri(a, d, b); }
      else if (n == 3) {
        if (!ia) tri(b, c, d);
        else if (!ib) tri(a, c, d);
        else if (!ic) tri(a, d, b);
        else tri(a, c, b);
      }
    }
  if (tf.empty()) { error = "mesh_inflate(): the mask is too thin to make a surface of (a line a cell wide)"; return Value::nil(); }

  // normals: the faces' area-weighted, summed at shared vertices (so the rim's point outward in the plane)
  std::vector<float> N(verts.size() * 3, 0.0f);
  auto acc = [&](const std::vector<uint32_t>& t) {
    for (size_t k = 0; k + 2 < t.size(); k += 3) {
      const V &p = verts[t[k]], &q = verts[t[k + 1]], &r = verts[t[k + 2]];
      const float ux = q.x - p.x, uy = q.y - p.y, uz = q.z - p.z, wx = r.x - p.x, wy = r.y - p.y, wz = r.z - p.z;
      const float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
      for (int j = 0; j < 3; ++j) { N[t[k + j] * 3] += nx; N[t[k + j] * 3 + 1] += ny; N[t[k + j] * 3 + 2] += nz; }
    }
  };
  acc(tf);
  acc(tb);

  // the r3d "z"-up frame: front +x (the mesh's +z), the picture's right +y, up +z
  auto half = [&](const std::vector<uint32_t>& t, bool front) {
    std::vector<int> remap(verts.size(), -1);
    std::vector<uint8_t> P, NN, U, I;
    int n = 0;
    for (uint32_t gi : t) {
      if (remap[gi] < 0) {
        remap[gi] = n++;
        const V& v = verts[gi];
        put_f32(P, v.z); put_f32(P, v.x); put_f32(P, v.y);
        float nx = N[gi * 3], ny = N[gi * 3 + 1], nz = N[gi * 3 + 2];
        const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (l > 0) { nx /= l; ny /= l; nz /= l; } else { nz = front ? 1.0f : -1.0f; }
        put_f32(NN, nz); put_f32(NN, nx); put_f32(NN, ny);
        put_f32(U, v.u); put_f32(U, v.v);
      }
      put_u32(I, (uint32_t)remap[gi]);
    }
    auto m = std::make_shared<MapObj>();
    m->put("vertex_count", Value::integer(n));
    m->put("positions", Value::bytes(std::move(P)));
    m->put("normals", Value::bytes(std::move(NN)));
    m->put("uvs", Value::bytes(std::move(U)));
    m->put("indices", Value::bytes(std::move(I)));
    return Value::object(Type::Map, m);
  };
  float zmax = 0;
  for (size_t k = 0; k < nfront; ++k) zmax = std::max(zmax, verts[k].z);
  auto res = std::make_shared<MapObj>();
  res->put("front", half(tf, true));
  res->put("back", half(tb, false));
  res->put("vertex_count", Value::integer((int64_t)verts.size()));
  res->put("tris", Value::integer((int64_t)((tf.size() + tb.size()) / 3)));
  res->put("width", Value::integer(w));
  res->put("height", Value::integer(ymax + 1));
  res->put("depth", Value::real(2.0 * zmax));
  return Value::object(Type::Map, res);
}

}  // namespace

void install_carve_builtins(Vm& vm) {
  vm.define_native("image_cutout", 1, 2, bi_image_cutout);
  vm.define_native("image_quantize", 2, 3, bi_image_quantize);
  vm.define_native("image_resample", 3, 4, bi_image_resample);
  vm.define_native("mesh_inflate", 3, 4, bi_mesh_inflate);
}

}  // namespace aios::userland::grain
