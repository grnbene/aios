// Carving game assets out of pictures: a photo's subject cut from its background, its colours put on a game's
// palette, and its silhouette inflated into a closed, rounded mesh - natively, so a phone snapshot becomes a sprite or
// a model in the time of a request (apps/sqldoom/lib/carve.gr builds Doom's monsters from them).
//
//   image_cutout(canvas[, opts])  -> {canvas, mask, w, h, bbox: {x, y, w, h}, coverage, threshold, background}
//       The subject of a photo, framed inside its border: GrabCut - colour models of subject and background
//       (mixtures of Gaussians in CIELAB), and the least costly cut between them, each cell paying for its side under
//       the models and every pair of neighbours put apart paying more the more alike they look; found as a minimum
//       cut (Boykov-Kolmogorov) and the models learnt again from it, a few rounds. A band round the border is taken
//       as background. Then the largest piece is kept, its holes filled, and the edge feathered over one pixel.
//       canvas: the picture with every pixel outside the subject made transparent; mask: bytes, one 0..255 a pixel
//       (top row first). opts: {margin (the band, a share of the shorter side; 0.025), background: [r g b] (cells
//       of about this colour joined to the border are background too), iterations (5), smoothness (50), keep:
//       "largest" | "all", max_side (the analysis size; 512)}. A picture where nothing stands out from the border's
//       colours is refused.
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
//
// Measuring from photos (a printed marker gives the scale), and tires against bins:
//   image_find_marker(canvas[, opts]) -> {corners: [[x y] x4], centre: [x y], side_px, score} or nil
//       The calibration marker: a black square with a white square half its side in the middle, on a lighter
//       ground. Found by an adaptive threshold, the dark pieces with one hole of about a quarter of their area
//       centred in them, a quadrilateral fitted to the outer edge and each side refined by a straight line through its
//       edge pixels; corners clockwise from the top left, in the picture's pixels. opts: {max_side (1200)}.
//   homography(from, to) -> [9 numbers]: the plane-to-plane map taking four points onto four (row by row, h33 = 1)
//   homography_apply(H, points) -> points: [[x y] ...] mapped by H
//   image_rectify(canvas, quad, w, h) -> canvas: the quadrilateral (four corners, clockwise from the top left) of the
//       picture made a w x h rectangle (each pixel sampled bilinearly through the homography)
//   tire_size(size) -> {width_mm, diameter_mm} or nil: a tire size ("225/45R17", "LT265/70R17", "33X12.50R15")
//   tire_fit(w, d, h, size | width_mm[, diameter_mm]) -> {count, mode}: how many fit a w x d x h bin (mm), lying in
//       stacks ("flat") or standing ("across", "deep") - aios/userland/tire_fit.hpp, as ASQL's TIRE_FIT
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <array>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "aios/userland/grain.hpp"
#include "aios/userland/tire_fit.hpp"
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

// ---- GrabCut's parts: a Gaussian mixture, and a maximum flow ------------------------------------------------

// A mixture of Gaussians over CIELAB colours (full covariance): fitted by k-means and one round of assigning each
// sample to its likeliest component; cost(c) is -log of the mixture's density at c.
struct Gmm {
  struct Comp { double w = 0, m[3] = {0, 0, 0}, inv[9] = {0}, logdet = 0; };
  std::vector<Comp> comps;
  static double v(const Lab& c, int k) { return k == 0 ? c.l : k == 1 ? c.a : c.b; }
  double comp_cost(const Comp& q, const Lab& c) const {
    const double d[3] = {c.l - q.m[0], c.a - q.m[1], c.b - q.m[2]};
    double m = 0;
    for (int r = 0; r < 3; ++r)
      for (int k = 0; k < 3; ++k) m += d[r] * q.inv[r * 3 + k] * d[k];
    return 0.5 * m + 0.5 * q.logdet;
  }
  double cost(const Lab& c) const {
    // -log sum_k w_k N(c): with the smallest exponent factored out, so far colours do not underflow to zero
    double best = std::numeric_limits<double>::max();
    std::vector<double> e(comps.size());
    for (size_t k = 0; k < comps.size(); ++k) { e[k] = comp_cost(comps[k], c) - std::log(comps[k].w); best = std::min(best, e[k]); }
    double s = 0;
    for (double x : e) s += std::exp(best - x);
    return best - std::log(s);
  }
  void fit(const std::vector<Lab>& s, int K) {
    // k-means on a sample for the start
    std::vector<Lab> sample;
    const size_t step = std::max<size_t>(1, s.size() / 20000);
    for (size_t i = 0; i < s.size(); i += step) sample.push_back(s[i]);
    const std::vector<Cluster> cs = kmeans(sample, K);
    std::vector<Comp> start;
    for (const Cluster& c : cs) {
      Comp q;
      q.w = 1.0 / cs.size();
      q.m[0] = c.c.l; q.m[1] = c.c.a; q.m[2] = c.c.b;
      for (int k = 0; k < 3; ++k) q.inv[k * 4] = 1.0;
      start.push_back(q);
    }
    comps = start;
    // each sample to its nearest centre, then each component's weight, mean and covariance from its samples
    std::vector<double> sum(comps.size() * 3, 0), sq(comps.size() * 9, 0);
    std::vector<size_t> cnt(comps.size(), 0);
    for (const Lab& c : s) {
      size_t bi = 0;
      double bd = std::numeric_limits<double>::max();
      for (size_t k = 0; k < comps.size(); ++k) {
        const double dl = c.l - comps[k].m[0], da = c.a - comps[k].m[1], db = c.b - comps[k].m[2];
        const double d = dl * dl + da * da + db * db;
        if (d < bd) { bd = d; bi = k; }
      }
      ++cnt[bi];
      for (int r = 0; r < 3; ++r) {
        sum[bi * 3 + r] += v(c, r);
        for (int k = 0; k < 3; ++k) sq[bi * 9 + r * 3 + k] += v(c, r) * v(c, k);
      }
    }
    std::vector<Comp> out;
    for (size_t k = 0; k < comps.size(); ++k) {
      if (cnt[k] == 0) continue;
      Comp q;
      q.w = (double)cnt[k] / s.size();
      for (int r = 0; r < 3; ++r) q.m[r] = sum[k * 3 + r] / cnt[k];
      double C[9];
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) C[r * 3 + c] = sq[k * 9 + r * 3 + c] / cnt[k] - q.m[r] * q.m[c];
      // a floor under each variance (one ΔE unit): a flat colour does not make a singular component
      for (int r = 0; r < 3; ++r) C[r * 4] += 1.0;
      const double det = C[0] * (C[4] * C[8] - C[5] * C[7]) - C[1] * (C[3] * C[8] - C[5] * C[6]) + C[2] * (C[3] * C[7] - C[4] * C[6]);
      if (!(det > 1e-12)) continue;
      q.inv[0] = (C[4] * C[8] - C[5] * C[7]) / det; q.inv[1] = (C[2] * C[7] - C[1] * C[8]) / det; q.inv[2] = (C[1] * C[5] - C[2] * C[4]) / det;
      q.inv[3] = (C[5] * C[6] - C[3] * C[8]) / det; q.inv[4] = (C[0] * C[8] - C[2] * C[6]) / det; q.inv[5] = (C[2] * C[3] - C[0] * C[5]) / det;
      q.inv[6] = (C[3] * C[7] - C[4] * C[6]) / det; q.inv[7] = (C[1] * C[6] - C[0] * C[7]) / det; q.inv[8] = (C[0] * C[4] - C[1] * C[3]) / det;
      q.logdet = std::log(det);
      out.push_back(q);
    }
    if (!out.empty()) comps = std::move(out);
  }
};

// The maximum flow / minimum cut of a graph with a source and a sink (Boykov and Kolmogorov's algorithm: two
// search trees grown from the terminals, augmented where they meet, orphans re-adopted), for the grid graphs of
// vision, where it is far faster than the general algorithms.
class Maxflow {
 public:
  Maxflow(size_t nodes, size_t edges_hint) : nodes_(nodes) { arcs_.reserve(edges_hint * 2); }
  void set_tweights(int i, double to_source, double to_sink) { nodes_[(size_t)i].tr_cap = to_source - to_sink; }
  void add_edge(int i, int j, double cap, double rev_cap) {
    const int a = (int)arcs_.size();
    arcs_.push_back({j, nodes_[(size_t)i].first, a + 1, cap});
    nodes_[(size_t)i].first = a;
    arcs_.push_back({i, nodes_[(size_t)j].first, a, rev_cap});
    nodes_[(size_t)j].first = a + 1;
  }
  bool source_side(int i) const { const Node& v = nodes_[(size_t)i]; return v.parent != NONE && !v.sink; }
  void run() {
    for (size_t i = 0; i < nodes_.size(); ++i) {
      Node& v = nodes_[i];
      if (v.tr_cap > 0) { v.sink = false; v.parent = TERMINAL; activate((int)i); v.ts = 0; v.dist = 1; }
      else if (v.tr_cap < 0) { v.sink = true; v.parent = TERMINAL; activate((int)i); v.ts = 0; v.dist = 1; }
      else v.parent = NONE;
    }
    int cur = -1;
    while (true) {
      // the next active node (one may have left the trees since it was queued)
      if (cur < 0 || nodes_[(size_t)cur].parent == NONE) {
        cur = -1;
        while (!active_.empty()) {
          const int i = active_.front();
          active_.pop_front();
          nodes_[(size_t)i].queued = false;
          if (nodes_[(size_t)i].parent != NONE) { cur = i; break; }
        }
        if (cur < 0) break;
      }
      Node& v = nodes_[(size_t)cur];
      int meet = -1;   // an arc from the source tree to the sink tree
      if (!v.sink) {
        for (int a = v.first; a >= 0; a = arcs_[(size_t)a].next) {
          if (arcs_[(size_t)a].r_cap <= 0) continue;
          Node& u = nodes_[(size_t)arcs_[(size_t)a].head];
          if (u.parent == NONE) { u.sink = false; u.parent = arcs_[(size_t)a].sister; u.ts = v.ts; u.dist = v.dist + 1; activate(arcs_[(size_t)a].head); }
          else if (u.sink) { meet = a; break; }
          else if (u.ts <= v.ts && u.dist > v.dist) { u.parent = arcs_[(size_t)a].sister; u.ts = v.ts; u.dist = v.dist + 1; }
        }
      } else {
        for (int a = v.first; a >= 0; a = arcs_[(size_t)a].next) {
          if (arcs_[(size_t)arcs_[(size_t)a].sister].r_cap <= 0) continue;
          Node& u = nodes_[(size_t)arcs_[(size_t)a].head];
          if (u.parent == NONE) { u.sink = true; u.parent = arcs_[(size_t)a].sister; u.ts = v.ts; u.dist = v.dist + 1; activate(arcs_[(size_t)a].head); }
          else if (!u.sink) { meet = arcs_[(size_t)a].sister; break; }
          else if (u.ts <= v.ts && u.dist > v.dist) { u.parent = arcs_[(size_t)a].sister; u.ts = v.ts; u.dist = v.dist + 1; }
        }
      }
      ++time_;
      if (meet < 0) { cur = -1; continue; }
      // keep growing from this node after the augmentation, if it is still in a tree
      augment(meet);
      adopt();
      if (nodes_[(size_t)cur].parent == NONE) cur = -1;
    }
  }

 private:
  static constexpr int NONE = -1, TERMINAL = -2, ORPHAN = -3;
  struct Node { int first = -1, parent = NONE, ts = 0, dist = 0; double tr_cap = 0; bool sink = false, queued = false; };
  struct Arc { int head, next, sister; double r_cap; };
  std::vector<Node> nodes_;
  std::vector<Arc> arcs_;
  std::deque<int> active_, orphans_;
  int time_ = 0;

  void activate(int i) { if (!nodes_[(size_t)i].queued) { nodes_[(size_t)i].queued = true; active_.push_back(i); } }
  int tail(int a) const { return arcs_[(size_t)arcs_[(size_t)a].sister].head; }
  void orphan(int i) { nodes_[(size_t)i].parent = ORPHAN; orphans_.push_back(i); }

  void augment(int meet) {
    double d = arcs_[(size_t)meet].r_cap;
    // the bottleneck up the source tree, then down to the sink
    for (int i = tail(meet);;) {
      const int a = nodes_[(size_t)i].parent;
      if (a == TERMINAL) { d = std::min(d, nodes_[(size_t)i].tr_cap); break; }
      d = std::min(d, arcs_[(size_t)arcs_[(size_t)a].sister].r_cap);
      i = arcs_[(size_t)a].head;
    }
    for (int i = arcs_[(size_t)meet].head;;) {
      const int a = nodes_[(size_t)i].parent;
      if (a == TERMINAL) { d = std::min(d, -nodes_[(size_t)i].tr_cap); break; }
      d = std::min(d, arcs_[(size_t)a].r_cap);
      i = arcs_[(size_t)a].head;
    }
    arcs_[(size_t)arcs_[(size_t)meet].sister].r_cap += d;
    arcs_[(size_t)meet].r_cap -= d;
    for (int i = tail(meet);;) {
      const int a = nodes_[(size_t)i].parent;
      if (a == TERMINAL) {
        nodes_[(size_t)i].tr_cap -= d;
        if (nodes_[(size_t)i].tr_cap <= 0) orphan(i);
        break;
      }
      arcs_[(size_t)a].r_cap += d;
      arcs_[(size_t)arcs_[(size_t)a].sister].r_cap -= d;
      const int up = arcs_[(size_t)a].head;
      if (arcs_[(size_t)arcs_[(size_t)a].sister].r_cap <= 0) orphan(i);
      i = up;
    }
    for (int i = arcs_[(size_t)meet].head;;) {
      const int a = nodes_[(size_t)i].parent;
      if (a == TERMINAL) {
        nodes_[(size_t)i].tr_cap += d;
        if (nodes_[(size_t)i].tr_cap >= 0) orphan(i);
        break;
      }
      arcs_[(size_t)arcs_[(size_t)a].sister].r_cap += d;
      arcs_[(size_t)a].r_cap -= d;
      const int up = arcs_[(size_t)a].head;
      if (arcs_[(size_t)a].r_cap <= 0) orphan(i);
      i = up;
    }
  }

  // how far i is from its terminal through valid parents (INT_MAX: its root is an orphan), marking the path
  int origin_dist(int j) {
    int d = 0;
    int k = j;
    while (true) {
      if (nodes_[(size_t)k].ts == time_) { d += nodes_[(size_t)k].dist; break; }
      const int a = nodes_[(size_t)k].parent;
      ++d;
      if (a == TERMINAL) { nodes_[(size_t)k].ts = time_; nodes_[(size_t)k].dist = 1; break; }
      if (a == ORPHAN || a == NONE) return std::numeric_limits<int>::max();
      k = arcs_[(size_t)a].head;
    }
    // the distances along the path, for the next searches
    int dd = d;
    for (k = j; nodes_[(size_t)k].ts != time_; k = arcs_[(size_t)nodes_[(size_t)k].parent].head) {
      nodes_[(size_t)k].ts = time_;
      nodes_[(size_t)k].dist = dd--;
    }
    return d;
  }

  void adopt() {
    while (!orphans_.empty()) {
      const int i = orphans_.front();
      orphans_.pop_front();
      Node& v = nodes_[(size_t)i];
      const bool sink = v.sink;
      int best = NONE, dmin = std::numeric_limits<int>::max();
      for (int a = v.first; a >= 0; a = arcs_[(size_t)a].next) {
        // a parent must be able to pass flow to i (source tree) or take it from i (sink tree)
        const double cap = sink ? arcs_[(size_t)a].r_cap : arcs_[(size_t)arcs_[(size_t)a].sister].r_cap;
        if (cap <= 0) continue;
        const int j = arcs_[(size_t)a].head;
        if (nodes_[(size_t)j].sink != sink || nodes_[(size_t)j].parent == NONE) continue;
        const int d = origin_dist(j);
        if (d < dmin) { dmin = d; best = a; }
      }
      if (best != NONE) {
        v.parent = best;
        v.ts = time_;
        v.dist = dmin + 1;
        continue;
      }
      // no parent: i leaves its tree; its neighbours that could reach it become active, its children orphans
      v.parent = NONE;
      for (int a = v.first; a >= 0; a = arcs_[(size_t)a].next) {
        const int j = arcs_[(size_t)a].head;
        Node& u = nodes_[(size_t)j];
        if (u.sink != sink || u.parent == NONE) continue;
        const double cap = sink ? arcs_[(size_t)a].r_cap : arcs_[(size_t)arcs_[(size_t)a].sister].r_cap;
        if (cap > 0) activate(j);
        if (u.parent != TERMINAL && u.parent != ORPHAN && arcs_[(size_t)u.parent].head == i) orphan(j);
      }
    }
  }
};

// ---- image_cutout ---------------------------------------------------------------------------------------------

Value bi_image_cutout(Vm&, std::vector<Value>& args, std::string& error) {
  uint32_t W = 0, H = 0;
  const std::vector<uint32_t>* px = nullptr;
  if (!canvas_pixels(args[0], &W, &H, &px, error, "image_cutout")) return Value::nil();
  if (W < 8 || H < 8) { error = "image_cutout(): the picture is smaller than 8x8"; return Value::nil(); }
  MapObj* o = opts_map(args, 1);
  const int max_side = (int)std::clamp(opt_num(o, "max_side", 512), 64.0, 2048.0);
  const int iterations = (int)std::clamp(opt_num(o, "iterations", 5), 1.0, 20.0);
  const double gamma = std::clamp(opt_num(o, "smoothness", 50), 0.0, 1000.0);
  const double margin = std::clamp(opt_num(o, "margin", 0.025), 0.002, 0.3);
  bool keep_all = false;
  if (o) if (const Value* k = o->find("keep"); k && k->type == Type::Str) {
    if (k->str() == "all") keep_all = true;
    else if (k->str() != "largest") { error = "image_cutout(): keep is \"largest\" or \"all\", not \"" + k->str() + "\""; return Value::nil(); }
  }

  // the analysis picture: box-filtered down to max_side, each cell in CIELAB
  const double sc = std::max(1.0, (double)std::max(W, H) / max_side);
  const int w = std::max(8, (int)std::lround(W / sc)), h = std::max(8, (int)std::lround(H / sc));
  const size_t n = (size_t)w * h;
  std::vector<Lab> L(n);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const int x0 = (int)(x * (double)W / w), x1 = std::max(x0 + 1, (int)((x + 1) * (double)W / w));
      const int y0 = (int)(y * (double)H / h), y1 = std::max(y0 + 1, (int)((y + 1) * (double)H / h));
      double r = 0, g = 0, b = 0;
      int c = 0;
      for (int yy = y0; yy < std::min<int>(y1, H); ++yy)
        for (int xx = x0; xx < std::min<int>(x1, W); ++xx) {
          const uint32_t p = (*px)[(size_t)yy * W + xx];
          r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255; ++c;
        }
      L[(size_t)y * w + x] = to_lab((uint8_t)(r / c + 0.5), (uint8_t)(g / c + 0.5), (uint8_t)(b / c + 0.5));
    }

  // what is certainly background: a band around the border (the subject is framed inside it), and with
  // opts.background, every cell of about that colour joined to the border
  const int band = std::max(1, (int)std::lround(std::min(w, h) * margin));
  std::vector<uint8_t> sure_bg(n, 0);
  std::vector<Lab> border;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      if (x < band || y < band || x >= w - band || y >= h - band) { sure_bg[(size_t)y * w + x] = 1; border.push_back(L[(size_t)y * w + x]); }
  if (o) if (const Value* bv = o->find("background"); bv && bv->type == Type::List && bv->list_items().size() >= 3) {
    const auto& it = bv->list_items();
    const Lab given = to_lab((uint8_t)std::clamp<int64_t>(it[0].as_int(), 0, 255), (uint8_t)std::clamp<int64_t>(it[1].as_int(), 0, 255),
                             (uint8_t)std::clamp<int64_t>(it[2].as_int(), 0, 255));
    std::vector<int> stack;
    for (size_t i = 0; i < n; ++i) if (sure_bg[i]) stack.push_back((int)i);
    while (!stack.empty()) {
      const int p = stack.back();
      stack.pop_back();
      const int x = p % w, y = p / w;
      const int nb[4] = {x > 0 ? p - 1 : -1, x < w - 1 ? p + 1 : -1, y > 0 ? p - w : -1, y < h - 1 ? p + w : -1};
      for (int q : nb)
        if (q >= 0 && !sure_bg[q] && dist2(L[q], given) < 64.0f) { sure_bg[q] = 1; stack.push_back(q); }
    }
  }

  // opts.background_mask: bytes at the picture's size, nonzero where it is certainly background (a cell is, when
  // most of what it covers is)
  if (o) if (const Value* bm = o->find("background_mask"); bm && bm->type != Type::Nil) {
    if (bm->type != Type::Bytes || bm->bytes_data().size() != (size_t)W * H) { error = "image_cutout(): background_mask wants bytes, one a pixel of the picture"; return Value::nil(); }
    const auto& B = bm->bytes_data();
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const int x0 = (int)(x * (double)W / w), x1 = std::max(x0 + 1, (int)((x + 1) * (double)W / w));
        const int y0 = (int)(y * (double)H / h), y1 = std::max(y0 + 1, (int)((y + 1) * (double)H / h));
        int on = 0, all = 0;
        for (int yy = y0; yy < std::min<int>(y1, H); ++yy)
          for (int xx = x0; xx < std::min<int>(x1, W); ++xx) { on += B[(size_t)yy * W + xx] != 0; ++all; }
        if (on * 2 > all) sure_bg[(size_t)y * w + x] = 1;
      }
  }
  // opts.foreground_mask: the same, nonzero where it is certainly the subject (a swipe along it)
  std::vector<uint8_t> sure_fg(n, 0);
  if (o) if (const Value* fm = o->find("foreground_mask"); fm && fm->type != Type::Nil) {
    if (fm->type != Type::Bytes || fm->bytes_data().size() != (size_t)W * H) { error = "image_cutout(): foreground_mask wants bytes, one a pixel of the picture"; return Value::nil(); }
    const auto& B = fm->bytes_data();
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const int x0 = (int)(x * (double)W / w), x1 = std::max(x0 + 1, (int)((x + 1) * (double)W / w));
        const int y0 = (int)(y * (double)H / h), y1 = std::max(y0 + 1, (int)((y + 1) * (double)H / h));
        bool any = false;
        for (int yy = y0; yy < std::min<int>(y1, H) && !any; ++yy)
          for (int xx = x0; xx < std::min<int>(x1, W) && !any; ++xx) any = B[(size_t)yy * W + xx] != 0;
        if (any) { sure_fg[(size_t)y * w + x] = 1; sure_bg[(size_t)y * w + x] = 0; }
      }
  }
  // whether anything stands out at all: how many cells are far (3 spreads) from every colour of the border
  const std::vector<Cluster> border_colours = kmeans(border, 5);
  {
    size_t far = 0;
    for (size_t i = 0; i < n; ++i) if (!sure_bg[i] && cluster_distance(L[i], border_colours, 2.5f) >= 3.0f) ++far;
    bool marked = false;
    for (uint8_t v : sure_fg) if (v) { marked = true; break; }
    if (far * 1000 < n && !marked) { error = "image_cutout(): nothing stands out from the background (a plainer background, or the subject filling more of the picture, helps)"; return Value::nil(); }
  }

  // GrabCut (Rother, Kolmogorov and Blake): colour models of the subject and the background (mixtures of five
  // Gaussians each), and the cut between them that costs least - each cell's cost of its side under the models, and
  // a cost for every pair of neighbours put on different sides, high where they look alike - found as a minimum cut,
  // the models learnt again from the cut, a few times over. It starts from "everything inside the band".
  std::vector<uint8_t> fg(n);
  for (size_t i = 0; i < n; ++i) fg[i] = sure_bg[i] ? 0 : 1;
  // the neighbours' weights: right, down, down-right, down-left; beta from the picture's mean contrast
  double mean_d2 = 0;
  size_t npairs = 0;
  const int dx4[4] = {1, 0, 1, -1}, dy4[4] = {0, 1, 1, 1};
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      for (int k = 0; k < 4; ++k) {
        const int nx = x + dx4[k], ny = y + dy4[k];
        if (nx < 0 || nx >= w || ny >= h) continue;
        mean_d2 += dist2(L[(size_t)y * w + x], L[(size_t)ny * w + nx]);
        ++npairs;
      }
  const double beta = npairs && mean_d2 > 0 ? 1.0 / (2.0 * mean_d2 / npairs) : 0.0;
  std::vector<float> nw(n * 4, 0.0f);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      for (int k = 0; k < 4; ++k) {
        const int nx = x + dx4[k], ny = y + dy4[k];
        if (nx < 0 || nx >= w || ny >= h) continue;
        const double d2 = dist2(L[(size_t)y * w + x], L[(size_t)ny * w + nx]);
        nw[((size_t)y * w + x) * 4 + k] = (float)(gamma / (k >= 2 ? std::sqrt(2.0) : 1.0) * std::exp(-beta * d2));
      }
  Gmm gfg, gbg;
  for (int it = 0; it < iterations; ++it) {
    std::vector<Lab> sf, sb;
    for (size_t i = 0; i < n; ++i) (fg[i] ? sf : sb).push_back(L[i]);
    if (sf.size() < 16 || sb.size() < 16) break;
    gfg.fit(sf, 5);
    gbg.fit(sb, 5);
    Maxflow g(n, n * 4);
    const double big = 1e9;
    for (size_t i = 0; i < n; ++i) {
      if (sure_bg[i]) { g.set_tweights((int)i, 0, big); continue; }
      if (sure_fg[i]) { g.set_tweights((int)i, big, 0); continue; }
      // the source side is the subject: its capacity is the cost of calling the cell background, and the other way
      g.set_tweights((int)i, gbg.cost(L[i]), gfg.cost(L[i]));
    }
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        for (int k = 0; k < 4; ++k) {
          const int nx = x + dx4[k], ny = y + dy4[k];
          if (nx < 0 || nx >= w || ny >= h) continue;
          const float c = nw[((size_t)y * w + x) * 4 + k];
          if (c > 0) g.add_edge(y * w + x, ny * w + nx, c, c);
        }
    g.run();
    size_t changed = 0;
    for (size_t i = 0; i < n; ++i) {
      const uint8_t v = sure_bg[i] ? 0 : (sure_fg[i] || g.source_side((int)i) ? 1 : 0);
      changed += v != fg[i];
      fg[i] = v;
    }
    if (changed * 2000 < n) break;   // settled
  }

  // the subject's piece(s) kept, specks of either side gone, holes filled
  auto keep_pieces = [&](std::vector<uint8_t>& m) {
    std::vector<int> lab;
    std::vector<size_t> sizes;
    const int np = label_pieces(m, w, h, lab, sizes);
    if (np == 0) return;
    const size_t biggest = *std::max_element(sizes.begin(), sizes.end());
    for (size_t i = 0; i < m.size(); ++i) {
      if (lab[i] < 0) continue;
      const size_t s = sizes[(size_t)lab[i]];
      m[i] = keep_all ? (s * 50 >= biggest ? 1 : 0) : (s == biggest ? 1 : 0);
    }
  };
  const int open_r = std::max(1, std::min(w, h) / 256);
  morph(fg, w, h, open_r, false);
  morph(fg, w, h, open_r, true);
  keep_pieces(fg);
  fill_holes(fg, w, h);
  size_t nfg = 0;
  for (uint8_t v : fg) nfg += v;
  if (nfg == 0) { error = "image_cutout(): no subject found inside the border (a plainer background helps)"; return Value::nil(); }
  const float thr = 3.0f;   // (reported: the score a cell had to reach to count as standing out)

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

// ---- measuring: the marker, homographies -------------------------------------------------------------------

struct P2 { double x, y; };

// the 3x3 homography taking a[i] to b[i] (four pairs): the 8x8 linear system solved with partial pivoting
bool solve_homography(const P2* a, const P2* b, double H[9]) {
  double M[8][9];
  for (int i = 0; i < 4; ++i) {
    const double x = a[i].x, y = a[i].y, u = b[i].x, v = b[i].y;
    double r0[9] = {x, y, 1, 0, 0, 0, -u * x, -u * y, u};
    double r1[9] = {0, 0, 0, x, y, 1, -v * x, -v * y, v};
    std::memcpy(M[i * 2], r0, sizeof r0);
    std::memcpy(M[i * 2 + 1], r1, sizeof r1);
  }
  for (int c = 0; c < 8; ++c) {
    int piv = c;
    for (int r = c + 1; r < 8; ++r) if (std::fabs(M[r][c]) > std::fabs(M[piv][c])) piv = r;
    if (std::fabs(M[piv][c]) < 1e-12) return false;
    if (piv != c) for (int k = 0; k < 9; ++k) std::swap(M[c][k], M[piv][k]);
    for (int r = 0; r < 8; ++r) {
      if (r == c) continue;
      const double f = M[r][c] / M[c][c];
      for (int k = c; k < 9; ++k) M[r][k] -= f * M[c][k];
    }
  }
  for (int i = 0; i < 8; ++i) H[i] = M[i][8] / M[i][i];
  H[8] = 1;
  return true;
}
P2 apply_h(const double H[9], P2 p) {
  const double w = H[6] * p.x + H[7] * p.y + H[8];
  return {(H[0] * p.x + H[1] * p.y + H[2]) / w, (H[3] * p.x + H[4] * p.y + H[5]) / w};
}

bool read_points(const Value& v, std::vector<P2>& out, size_t want, std::string& error, const char* who) {
  out.clear();
  if (v.type != Type::List) { error = std::string(who) + "() wants a list of [x, y] points"; return false; }
  for (const Value& p : v.list_items()) {
    if (p.type != Type::List || p.list_items().size() < 2 || !p.list_items()[0].is_number() || !p.list_items()[1].is_number()) {
      error = std::string(who) + "() wants each point as [x, y]";
      return false;
    }
    out.push_back({p.list_items()[0].as_double(), p.list_items()[1].as_double()});
  }
  if (want && out.size() != want) { error = std::string(who) + "() wants " + std::to_string(want) + " points, got " + std::to_string(out.size()); return false; }
  return true;
}
Value points_value(const std::vector<P2>& ps) {
  std::vector<Value> out;
  for (const P2& p : ps) out.push_back(Value::list({Value::real(p.x), Value::real(p.y)}));
  return Value::list(std::move(out));
}
bool read_h(const Value& v, double H[9], std::string& error, const char* who) {
  if (v.type != Type::List || v.list_items().size() != 9) { error = std::string(who) + "() wants a homography: a list of 9 numbers"; return false; }
  for (int i = 0; i < 9; ++i) {
    if (!v.list_items()[(size_t)i].is_number()) { error = std::string(who) + "() wants a homography: a list of 9 numbers"; return false; }
    H[i] = v.list_items()[(size_t)i].as_double();
  }
  return true;
}

Value bi_homography(Vm&, std::vector<Value>& args, std::string& error) {
  std::vector<P2> a, b;
  if (!read_points(args[0], a, 4, error, "homography") || !read_points(args[1], b, 4, error, "homography")) return Value::nil();
  double H[9];
  if (!solve_homography(a.data(), b.data(), H)) { error = "homography(): the points are degenerate (three in a line)"; return Value::nil(); }
  std::vector<Value> out;
  for (double h : H) out.push_back(Value::real(h));
  return Value::list(std::move(out));
}

Value bi_homography_apply(Vm&, std::vector<Value>& args, std::string& error) {
  double H[9];
  if (!read_h(args[0], H, error, "homography_apply")) return Value::nil();
  std::vector<P2> ps;
  if (!read_points(args[1], ps, 0, error, "homography_apply")) return Value::nil();
  for (P2& p : ps) p = apply_h(H, p);
  return points_value(ps);
}

Value bi_image_rectify(Vm&, std::vector<Value>& args, std::string& error) {
  uint32_t W = 0, H = 0;
  const std::vector<uint32_t>* px = nullptr;
  if (!canvas_pixels(args[0], &W, &H, &px, error, "image_rectify")) return Value::nil();
  std::vector<P2> q;
  if (!read_points(args[1], q, 4, error, "image_rectify")) return Value::nil();
  if (!want_number(args[2], error, "image_rectify") || !want_number(args[3], error, "image_rectify")) return Value::nil();
  const int64_t ow = args[2].as_int(), oh = args[3].as_int();
  if (ow < 1 || oh < 1 || ow > 8192 || oh > 8192) { error = "image_rectify(): the size is 1..8192 each way"; return Value::nil(); }
  const P2 rect[4] = {{0, 0}, {(double)ow, 0}, {(double)ow, (double)oh}, {0, (double)oh}};
  double M[9];
  if (!solve_homography(rect, q.data(), M)) { error = "image_rectify(): the quadrilateral is degenerate"; return Value::nil(); }
  std::vector<uint32_t> out((size_t)ow * oh, 0);
  for (int64_t y = 0; y < oh; ++y)
    for (int64_t x = 0; x < ow; ++x) {
      const P2 s = apply_h(M, {x + 0.5, y + 0.5});
      const double fx = s.x - 0.5, fy = s.y - 0.5;
      if (fx < -0.5 || fy < -0.5 || fx > W - 0.5 || fy > H - 0.5) continue;
      const int x0 = std::clamp((int)std::floor(fx), 0, (int)W - 1), y0 = std::clamp((int)std::floor(fy), 0, (int)H - 1);
      const int x1 = std::min<int>(x0 + 1, W - 1), y1 = std::min<int>(y0 + 1, H - 1);
      const double tx = std::clamp(fx - x0, 0.0, 1.0), ty = std::clamp(fy - y0, 0.0, 1.0);
      uint32_t c = 0;
      for (int sh = 0; sh < 32; sh += 8) {
        auto ch = [&](int xx, int yy) { return (double)(((*px)[(size_t)yy * W + xx] >> sh) & 255); };
        const double v = (ch(x0, y0) * (1 - tx) + ch(x1, y0) * tx) * (1 - ty) + (ch(x0, y1) * (1 - tx) + ch(x1, y1) * tx) * ty;
        c |= (uint32_t)std::clamp(std::lround(v), 0L, 255L) << sh;
      }
      out[(size_t)y * ow + x] = c;
    }
  return canvas_value((uint32_t)ow, (uint32_t)oh, std::move(out));
}

// a line through points by total least squares: a point on it and its direction
bool fit_line(const std::vector<P2>& ps, P2* at, P2* dir) {
  if (ps.size() < 2) return false;
  double mx = 0, my = 0;
  for (const P2& p : ps) { mx += p.x; my += p.y; }
  mx /= ps.size(); my /= ps.size();
  double sxx = 0, sxy = 0, syy = 0;
  for (const P2& p : ps) { sxx += (p.x - mx) * (p.x - mx); sxy += (p.x - mx) * (p.y - my); syy += (p.y - my) * (p.y - my); }
  const double ang = 0.5 * std::atan2(2 * sxy, sxx - syy);
  *at = {mx, my};
  *dir = {std::cos(ang), std::sin(ang)};
  return true;
}
bool meet(P2 a, P2 da, P2 b, P2 db, P2* out) {
  const double den = da.x * db.y - da.y * db.x;
  if (std::fabs(den) < 1e-9) return false;
  const double t = ((b.x - a.x) * db.y - (b.y - a.y) * db.x) / den;
  *out = {a.x + da.x * t, a.y + da.y * t};
  return true;
}
double seg_dist(P2 p, P2 a, P2 b) {
  const double vx = b.x - a.x, vy = b.y - a.y, l2 = vx * vx + vy * vy;
  const double t = l2 > 0 ? std::clamp(((p.x - a.x) * vx + (p.y - a.y) * vy) / l2, 0.0, 1.0) : 0.0;
  return std::hypot(p.x - a.x - t * vx, p.y - a.y - t * vy);
}
double quad_area(const P2* q) {
  double a = 0;
  for (int i = 0; i < 4; ++i) a += q[i].x * q[(i + 1) % 4].y - q[(i + 1) % 4].x * q[i].y;
  return std::fabs(a) / 2;
}

// A dark quadrilateral with holes in it (a printed marker of either kind): its corners in the picture's pixels,
// clockwise from the top left, refined along each side; how much of it its holes are, how far off its centre they
// sit, and whether they are bright (the paper's white) against it.
struct DarkQuad { P2 c[4]; double side = 0, fill = 0, hole_share = 0, off = 0; bool bright = false; };

std::vector<DarkQuad> find_dark_quads(const std::vector<uint32_t>* px, uint32_t W, uint32_t H, int max_side) {
  const double sc = std::max(1.0, (double)std::max(W, H) / max_side);
  const int w = std::max(8, (int)std::lround(W / sc)), h = std::max(8, (int)std::lround(H / sc));
  const size_t n = (size_t)w * h;
  // luma, box-filtered down
  std::vector<float> g(n);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const int x0 = (int)(x * (double)W / w), x1 = std::max(x0 + 1, (int)((x + 1) * (double)W / w));
      const int y0 = (int)(y * (double)H / h), y1 = std::max(y0 + 1, (int)((y + 1) * (double)H / h));
      double s = 0;
      int c = 0;
      for (int yy = y0; yy < std::min<int>(y1, H); ++yy)
        for (int xx = x0; xx < std::min<int>(x1, W); ++xx) {
          const uint32_t p = (*px)[(size_t)yy * W + xx];
          s += 0.299 * ((p >> 16) & 255) + 0.587 * ((p >> 8) & 255) + 0.114 * (p & 255);
          ++c;
        }
      g[(size_t)y * w + x] = (float)(s / c);
    }
  // adaptive threshold: darker than its neighbourhood's mean (an integral image)
  std::vector<double> I((size_t)(w + 1) * (h + 1), 0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      I[(size_t)(y + 1) * (w + 1) + x + 1] = g[(size_t)y * w + x] + I[(size_t)y * (w + 1) + x + 1] + I[(size_t)(y + 1) * (w + 1) + x] - I[(size_t)y * (w + 1) + x];
  const int r = std::max(7, std::min(w, h) / 12);
  // and darker than the picture's own split between dark and light (Otsu): inside a large dark area the local mean is
  // dark too, and the adaptive test alone would hollow it into a ring that looks like a marker
  double otsu = 0;
  {
    std::vector<double> hist(256, 0);
    for (float v : g) hist[std::clamp((int)v, 0, 255)] += 1;
    double sum = 0, wb = 0, sb = 0, best = -1;
    for (int i = 0; i < 256; ++i) sum += i * hist[i];
    for (int t = 0; t < 256; ++t) {
      wb += hist[t];
      if (wb == 0) continue;
      const double wf = (double)n - wb;
      if (wf == 0) break;
      sb += t * hist[t];
      const double mb = sb / wb, mf = (sum - sb) / wf, var = wb * wf * (mb - mf) * (mb - mf);
      if (var > best) { best = var; otsu = t + 0.5; }
    }
  }
  std::vector<uint8_t> dark(n, 0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const int xa = std::max(0, x - r), xb = std::min(w, x + r + 1), ya = std::max(0, y - r), yb = std::min(h, y + r + 1);
      const double s = I[(size_t)yb * (w + 1) + xb] - I[(size_t)ya * (w + 1) + xb] - I[(size_t)yb * (w + 1) + xa] + I[(size_t)ya * (w + 1) + xa];
      const double mean = s / ((double)(xb - xa) * (yb - ya));
      const float v = g[(size_t)y * w + x];
      dark[(size_t)y * w + x] = (v < mean - 12.0 && v < mean * 0.82) || v < otsu;
    }
  std::vector<int> lab;
  std::vector<size_t> sizes;
  label_pieces(dark, w, h, lab, sizes);
  std::vector<DarkQuad> out;
  for (size_t id = 0; id < sizes.size(); ++id) {
    if (sizes[id] < std::max<size_t>(64, n / 4000)) continue;
    // its bounding box, a cell bigger
    int bx0 = w, by0 = h, bx1 = -1, by1 = -1;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        if (lab[(size_t)y * w + x] == (int)id) { bx0 = std::min(bx0, x); by0 = std::min(by0, y); bx1 = std::max(bx1, x); by1 = std::max(by1, y); }
    if (bx0 <= 0 || by0 <= 0 || bx1 >= w - 1 || by1 >= h - 1) continue;   // touching the picture's edge
    bx0 -= 1; by0 -= 1; bx1 += 1; by1 += 1;
    const int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
    // what is not the piece, reached from the box's edge, is outside; the rest is its holes
    std::vector<uint8_t> outside((size_t)bw * bh, 0);
    std::vector<int> st;
    auto in_piece = [&](int x, int y) { return lab[(size_t)(y + by0) * w + x + bx0] == (int)id; };
    auto seed = [&](int x, int y) { if (!in_piece(x, y) && !outside[(size_t)y * bw + x]) { outside[(size_t)y * bw + x] = 1; st.push_back(y * bw + x); } };
    for (int x = 0; x < bw; ++x) { seed(x, 0); seed(x, bh - 1); }
    for (int y = 0; y < bh; ++y) { seed(0, y); seed(bw - 1, y); }
    while (!st.empty()) {
      const int p = st.back();
      st.pop_back();
      const int x = p % bw, y = p / bw;
      if (x > 0) seed(x - 1, y);
      if (x < bw - 1) seed(x + 1, y);
      if (y > 0) seed(x, y - 1);
      if (y < bh - 1) seed(x, y + 1);
    }
    double hole = 0, hx = 0, hy = 0, hole_g = 0, piece_g = 0;
    std::vector<P2> edge;
    double cx = 0, cy = 0;
    for (int y = 0; y < bh; ++y)
      for (int x = 0; x < bw; ++x) {
        if (in_piece(x, y)) {
          cx += x; cy += y; piece_g += g[(size_t)(y + by0) * w + x + bx0];
          const bool rim = (x > 0 && outside[(size_t)y * bw + x - 1]) || (x < bw - 1 && outside[(size_t)y * bw + x + 1]) ||
                           (y > 0 && outside[(size_t)(y - 1) * bw + x]) || (y < bh - 1 && outside[(size_t)(y + 1) * bw + x]);
          if (rim) edge.push_back({x + 0.5, y + 0.5});
        } else if (!outside[(size_t)y * bw + x]) {
          hole += 1; hx += x; hy += y; hole_g += g[(size_t)(y + by0) * w + x + bx0];
        }
      }
    if (hole < 16 || edge.size() < 16) continue;
    cx /= sizes[id]; cy /= sizes[id]; hx /= hole; hy /= hole;
    // the hole is the print's white: well lighter than its ring
    const bool bright = hole_g / hole >= piece_g / sizes[id] + 40.0;
    // the quadrilateral: the edge point farthest from the centre, the one farthest from it, then the farthest from
    // that diagonal on either side
    P2 q[4];
    auto far_from = [&](P2 a) { size_t b = 0; double bd = -1; for (size_t i = 0; i < edge.size(); ++i) { double d = std::hypot(edge[i].x - a.x, edge[i].y - a.y); if (d > bd) { bd = d; b = i; } } return edge[b]; };
    const P2 c0 = far_from({cx, cy}), c2 = far_from(c0);
    double best1 = 0, best3 = 0;
    P2 c1 = c0, c3 = c2;
    for (const P2& p : edge) {
      const double side = (c2.x - c0.x) * (p.y - c0.y) - (c2.y - c0.y) * (p.x - c0.x);
      if (side > best1) { best1 = side; c1 = p; }
      if (-side > best3) { best3 = -side; c3 = p; }
    }
    q[0] = c0; q[1] = c1; q[2] = c2; q[3] = c3;
    const double qa = quad_area(q);
    if (qa < 64) continue;
    const double fill = (sizes[id] + hole) / qa;
    const double hole_share = hole / qa;
    const double off = std::hypot(hx - cx, hy - cy) / std::sqrt(qa);
    // a filled quadrilateral (its holes in it): not a tire, not a shadow, not text
    if (fill < 0.85 || fill > 1.15) continue;
    // each side refined: a line through the edge points nearest it, away from the corners
    P2 at[4], dir[4];
    bool ok = true;
    for (int s = 0; s < 4 && ok; ++s) {
      const P2 a = q[s], b = q[(s + 1) % 4];
      const double len = std::hypot(b.x - a.x, b.y - a.y);
      std::vector<P2> mine;
      for (const P2& p : edge) {
        double dbest = seg_dist(p, a, b);
        bool nearest = true;
        for (int t = 0; t < 4 && nearest; ++t) if (t != s && seg_dist(p, q[t], q[(t + 1) % 4]) < dbest) nearest = false;
        if (!nearest) continue;
        if (std::hypot(p.x - a.x, p.y - a.y) < 0.12 * len || std::hypot(p.x - b.x, p.y - b.y) < 0.12 * len) continue;
        mine.push_back(p);
      }
      ok = fit_line(mine, &at[s], &dir[s]);
    }
    if (!ok) continue;
    P2 rq[4];
    for (int s = 0; s < 4 && ok; ++s) ok = meet(at[(s + 3) % 4], dir[(s + 3) % 4], at[s], dir[s], &rq[s]);
    if (!ok) continue;
    // the edge pixels' centres lie half a cell inside the edge: out by half a cell along the diagonals
    double mx = 0, my = 0;
    for (const P2& p : rq) { mx += p.x / 4; my += p.y / 4; }
    for (P2& p : rq) {
      const double dx = p.x - mx, dy = p.y - my, l = std::hypot(dx, dy);
      if (l > 0) { p.x += dx / l * 0.7071; p.y += dy / l * 0.7071; }
      p.x = (p.x + bx0) * sc;
      p.y = (p.y + by0) * sc;
    }
    // clockwise from the top left (smallest x + y)
    double ccx = 0, ccy = 0;
    for (const P2& p : rq) { ccx += p.x / 4; ccy += p.y / 4; }
    std::sort(rq, rq + 4, [&](const P2& a, const P2& b) { return std::atan2(a.y - ccy, a.x - ccx) < std::atan2(b.y - ccy, b.x - ccx); });
    int tl = 0;
    for (int i = 1; i < 4; ++i) if (rq[i].x + rq[i].y < rq[tl].x + rq[tl].y) tl = i;
    DarkQuad dq;
    for (int i = 0; i < 4; ++i) dq.c[i] = rq[(tl + i) % 4];
    dq.side = std::sqrt(quad_area(dq.c));
    dq.fill = fill;
    dq.hole_share = hole_share;
    dq.off = off;
    dq.bright = bright;
    out.push_back(dq);
  }
  return out;
}

Value bi_image_find_marker(Vm&, std::vector<Value>& args, std::string& error) {
  uint32_t W = 0, H = 0;
  const std::vector<uint32_t>* px = nullptr;
  if (!canvas_pixels(args[0], &W, &H, &px, error, "image_find_marker")) return Value::nil();
  MapObj* o = opts_map(args, 1);
  const int max_side = (int)std::clamp(opt_num(o, "max_side", 1200), 128.0, 4096.0);
  // the bin marker: a filled square with one bright centred hole of about a quarter
  struct Cand { P2 c[4]; double score; double side; };
  std::vector<Cand> cands;
  for (const DarkQuad& d : find_dark_quads(px, W, H, max_side)) {
    if (!d.bright || d.hole_share < 0.13 || d.hole_share > 0.42 || d.off > 0.12) continue;
    Cand cd;
    for (int i = 0; i < 4; ++i) cd.c[i] = d.c[i];
    cd.side = d.side;
    cd.score = (1.0 - std::fabs(d.fill - 1.0)) * (1.0 - std::fabs(d.hole_share - 0.25)) * (1.0 - d.off);
    cands.push_back(cd);
  }
  if (cands.empty()) return Value::nil();
  // the biggest that passed (a print fills more of the frame than anything that only looks like one)
  const Cand& best = *std::max_element(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.side < b.side; });
  auto res = std::make_shared<MapObj>();
  res->put("corners", points_value(std::vector<P2>(best.c, best.c + 4)));
  double mx = 0, my = 0;
  for (const P2& p : best.c) { mx += p.x / 4; my += p.y / 4; }
  res->put("centre", Value::list({Value::real(mx), Value::real(my)}));
  res->put("side_px", Value::real(best.side));
  res->put("score", Value::real(best.score));
  res->put("found", Value::integer((int64_t)cands.size()));
  return Value::object(Type::Map, res);
}

// ---- coded markers ------------------------------------------------------------------------------------------
//
// A marker that says which it is and which way up: 6 x 6 cells, the outer ring black, the inner 4 x 4 a code (1 a
// white cell) from a dictionary made here - each code differs from every other in every rotation by 5 cells or more
// and from its own rotations by 4 or more, so a misread cell is corrected and no marker is taken for another or
// turned. Ids 0 to 30 (the 31 codes the search finds).

int rot16(int code) {   // the inner 4 x 4 turned a quarter counter-clockwise: new[y][x] = old[x][3 - y]
  int out = 0;
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x)
      if (code >> (x * 4 + (3 - y)) & 1) out |= 1 << (y * 4 + x);
  return out;
}
int hamming16(int a, int b) { return __builtin_popcount((unsigned)(a ^ b) & 0xFFFF); }

const std::vector<int>& marker_codes() {
  static const std::vector<int> codes = [] {
    std::vector<int> out;
    uint32_t x = 2463534242u;   // a fixed walk through the 16-bit codes (xorshift), so the dictionary never changes
    for (int tries = 0; tries < 200000 && out.size() < 31; ++tries) {
      x ^= x << 13; x ^= x >> 17; x ^= x << 5;
      const int c = (int)(x & 0xFFFF);
      const int ones = __builtin_popcount((unsigned)c);
      if (ones < 5 || ones > 11) continue;
      int r = c, self = 99;
      for (int k = 1; k < 4; ++k) { r = rot16(r); self = std::min(self, hamming16(c, r)); }
      if (self < 4) continue;
      bool ok = true;
      for (int o : out) {
        int ro = o;
        for (int k = 0; k < 4 && ok; ++k) { if (hamming16(c, ro) < 5) ok = false; ro = rot16(ro); }
        if (!ok) break;
      }
      if (ok) out.push_back(c);
    }
    return out;
  }();
  return codes;
}

double luma_at(const std::vector<uint32_t>& px, uint32_t W, uint32_t H, double x, double y) {
  x = std::clamp(x - 0.5, 0.0, (double)W - 1); y = std::clamp(y - 0.5, 0.0, (double)H - 1);
  const int x0 = (int)x, y0 = (int)y, x1 = std::min<int>(x0 + 1, W - 1), y1 = std::min<int>(y0 + 1, H - 1);
  const double tx = x - x0, ty = y - y0;
  auto l = [&](int xx, int yy) { const uint32_t p = px[(size_t)yy * W + xx]; return 0.299 * ((p >> 16) & 255) + 0.587 * ((p >> 8) & 255) + 0.114 * (p & 255); };
  return (l(x0, y0) * (1 - tx) + l(x1, y0) * tx) * (1 - ty) + (l(x0, y1) * (1 - tx) + l(x1, y1) * tx) * ty;
}

Value bi_markers_find(Vm&, std::vector<Value>& args, std::string& error) {
  uint32_t W = 0, H = 0;
  const std::vector<uint32_t>* px = nullptr;
  if (!canvas_pixels(args[0], &W, &H, &px, error, "markers_find")) return Value::nil();
  MapObj* o = opts_map(args, 1);
  const int max_side = (int)std::clamp(opt_num(o, "max_side", 1600), 128.0, 4096.0);
  const auto& codes = marker_codes();
  struct Found { int id; P2 c[4]; double side; int errors; };
  std::vector<Found> found;
  for (const DarkQuad& d : find_dark_quads(px, W, H, max_side)) {
    if (d.hole_share < 0.04 || d.side < 12) continue;
    // the 6 x 6 cells read through the homography from the unit square onto the quadrilateral: each the mean of
    // nine samples about its middle
    const P2 unit[4] = {{0, 0}, {6, 0}, {6, 6}, {0, 6}};
    double M[9];
    if (!solve_homography(unit, d.c, M)) continue;
    double cell[36], lo = 1e9, hi = -1e9;
    for (int cy = 0; cy < 6; ++cy)
      for (int cx = 0; cx < 6; ++cx) {
        double sum = 0;
        for (int sy = -1; sy <= 1; ++sy)
          for (int sx = -1; sx <= 1; ++sx) {
            const P2 q = apply_h(M, {cx + 0.5 + sx * 0.22, cy + 0.5 + sy * 0.22});
            sum += luma_at(*px, W, H, q.x, q.y);
          }
        cell[cy * 6 + cx] = sum / 9;
        lo = std::min(lo, cell[cy * 6 + cx]); hi = std::max(hi, cell[cy * 6 + cx]);
      }
    if (hi - lo < 40) continue;
    const double t = (lo + hi) / 2;
    int ring_white = 0, bits = 0;
    for (int cy = 0; cy < 6; ++cy)
      for (int cx = 0; cx < 6; ++cx) {
        const bool white = cell[cy * 6 + cx] > t;
        if (cx == 0 || cy == 0 || cx == 5 || cy == 5) { ring_white += white; continue; }
        if (white) bits |= 1 << ((cy - 1) * 4 + (cx - 1));
      }
    if (ring_white > 1) continue;
    // which code, and turned how far: the observed cells turned k quarters counter-clockwise match it
    int best_id = -1, best_k = 0, best_e = 99, r = bits;
    for (int k = 0; k < 4; ++k) {
      for (size_t i = 0; i < codes.size(); ++i) {
        const int e = hamming16(r, codes[i]);
        if (e < best_e) { best_e = e; best_id = (int)i; best_k = k; }
      }
      r = rot16(r);
    }
    if (best_id < 0 || best_e > 1) continue;
    Found f;
    f.id = best_id;
    // the marker's own top-left is the observed corner k (turning counter-clockwise brings corner k to the top left)
    for (int i = 0; i < 4; ++i) f.c[i] = d.c[(best_k + i) % 4];
    f.side = d.side;
    f.errors = best_e + ring_white;
    found.push_back(f);
  }
  // one per id: the biggest (a marker seen twice is a reflection or a second print)
  std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.id != b.id ? a.id < b.id : a.side > b.side; });
  std::vector<Value> out;
  int last = -1;
  for (const Found& f : found) {
    if (f.id == last) continue;
    last = f.id;
    auto m = std::make_shared<MapObj>();
    m->put("id", Value::integer(f.id));
    m->put("corners", points_value(std::vector<P2>(f.c, f.c + 4)));
    double mx = 0, my = 0;
    for (const P2& p : f.c) { mx += p.x / 4; my += p.y / 4; }
    m->put("centre", Value::list({Value::real(mx), Value::real(my)}));
    m->put("side_px", Value::real(f.side));
    m->put("errors", Value::integer(f.errors));
    out.push_back(Value::object(Type::Map, m));
  }
  return Value::list(std::move(out));
}

// marker_image(id, px) -> canvas: the coded marker `id`, px pixels across its black square, with a white margin of
// a cell all round (what has to be printed: the white ring is part of it)
Value bi_marker_image(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "marker_image") || !want_number(args[1], error, "marker_image")) return Value::nil();
  const auto& codes = marker_codes();
  const int64_t id = args[0].as_int(), side = args[1].as_int();
  if (id < 0 || id >= (int64_t)codes.size()) { error = "marker_image(): ids are 0 to " + std::to_string(codes.size() - 1); return Value::nil(); }
  if (side < 12 || side > 8192) { error = "marker_image(): the square is 12 to 8192 pixels"; return Value::nil(); }
  const double cellpx = side / 6.0;
  const int margin = (int)std::lround(cellpx), total = (int)side + 2 * margin;
  std::vector<uint32_t> px((size_t)total * total, 0xFFFFFFFFu);
  for (int y = 0; y < (int)side; ++y)
    for (int x = 0; x < (int)side; ++x) {
      const int cx = std::min(5, (int)(x / cellpx)), cy = std::min(5, (int)(y / cellpx));
      bool white = false;
      if (cx > 0 && cy > 0 && cx < 5 && cy < 5) white = codes[(size_t)id] >> ((cy - 1) * 4 + (cx - 1)) & 1;
      px[(size_t)(y + margin) * total + x + margin] = white ? 0xFFFFFFFFu : 0xFF000000u;
    }
  return canvas_value((uint32_t)total, (uint32_t)total, std::move(px));
}

// ---- cameras ----------------------------------------------------------------------------------------------------
//
// A pinhole camera: a world point X goes to the camera's frame as R X + t (x right, y down, z ahead in the picture's
// sense) and to the picture at (cx + f x / z, cy + f y / z).

struct Cam { double f = 1, cx = 0, cy = 0, R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, t[3] = {0, 0, 0}; };

void rodrigues(const double v[3], double R[9]) {
  const double th = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (th < 1e-12) { const double I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; std::memcpy(R, I, sizeof I); return; }
  const double x = v[0] / th, y = v[1] / th, z = v[2] / th, c = std::cos(th), s = std::sin(th), C = 1 - c;
  const double M[9] = {c + x * x * C, x * y * C - z * s, x * z * C + y * s,
                       y * x * C + z * s, c + y * y * C, y * z * C - x * s,
                       z * x * C - y * s, z * y * C + x * s, c + z * z * C};
  std::memcpy(R, M, sizeof M);
}
void rodrigues_of(const double R[9], double v[3]) {
  const double c = std::clamp((R[0] + R[4] + R[8] - 1) / 2, -1.0, 1.0), th = std::acos(c);
  if (th < 1e-9) { v[0] = v[1] = v[2] = 0; return; }
  if (std::fabs(th - M_PI) < 1e-6) {
    // a half turn: the axis from the diagonal
    double x = std::sqrt(std::max(0.0, (R[0] + 1) / 2)), y = std::sqrt(std::max(0.0, (R[4] + 1) / 2)), z = std::sqrt(std::max(0.0, (R[8] + 1) / 2));
    if (R[1] < 0) y = -y;
    if (R[2] < 0) z = -z;
    v[0] = x * th; v[1] = y * th; v[2] = z * th;
    return;
  }
  const double k = th / (2 * std::sin(th));
  v[0] = (R[7] - R[5]) * k; v[1] = (R[2] - R[6]) * k; v[2] = (R[3] - R[1]) * k;
}
bool project(const Cam& c, const double X[3], P2* out, double* depth = nullptr) {
  const double x = c.R[0] * X[0] + c.R[1] * X[1] + c.R[2] * X[2] + c.t[0];
  const double y = c.R[3] * X[0] + c.R[4] * X[1] + c.R[5] * X[2] + c.t[1];
  const double z = c.R[6] * X[0] + c.R[7] * X[1] + c.R[8] * X[2] + c.t[2];
  if (depth) *depth = z;
  if (z <= 1e-9) return false;
  *out = {c.cx + c.f * x / z, c.cy + c.f * y / z};
  return true;
}

// the homography from plane points to picture points by least squares (normalized; h33 = 1)
bool fit_homography(const std::vector<P2>& a, const std::vector<P2>& b, double H[9]) {
  const size_t n = a.size();
  if (n < 4 || b.size() != n) return false;
  auto norm = [](const std::vector<P2>& p, double T[3]) {
    double mx = 0, my = 0;
    for (const P2& q : p) { mx += q.x; my += q.y; }
    mx /= p.size(); my /= p.size();
    double d = 0;
    for (const P2& q : p) d += std::hypot(q.x - mx, q.y - my);
    d /= p.size();
    const double s = d > 0 ? std::sqrt(2.0) / d : 1;
    T[0] = s; T[1] = -s * mx; T[2] = -s * my;
  };
  double Ta[3], Tb[3];
  norm(a, Ta); norm(b, Tb);
  double AtA[8][8] = {{0}}, Atb[8] = {0};
  for (size_t i = 0; i < n; ++i) {
    const double x = Ta[0] * a[i].x + Ta[1], y = Ta[0] * a[i].y + Ta[2], u = Tb[0] * b[i].x + Tb[1], v = Tb[0] * b[i].y + Tb[2];
    const double r0[8] = {x, y, 1, 0, 0, 0, -u * x, -u * y}, r1[8] = {0, 0, 0, x, y, 1, -v * x, -v * y};
    for (int p = 0; p < 8; ++p) {
      for (int q = 0; q < 8; ++q) AtA[p][q] += r0[p] * r0[q] + r1[p] * r1[q];
      Atb[p] += r0[p] * u + r1[p] * v;
    }
  }
  double M[8][9];
  for (int p = 0; p < 8; ++p) { for (int q = 0; q < 8; ++q) M[p][q] = AtA[p][q]; M[p][8] = Atb[p]; }
  for (int c = 0; c < 8; ++c) {
    int piv = c;
    for (int r = c + 1; r < 8; ++r) if (std::fabs(M[r][c]) > std::fabs(M[piv][c])) piv = r;
    if (std::fabs(M[piv][c]) < 1e-14) return false;
    if (piv != c) for (int k = 0; k < 9; ++k) std::swap(M[c][k], M[piv][k]);
    for (int r = 0; r < 8; ++r) {
      if (r == c) continue;
      const double f = M[r][c] / M[c][c];
      for (int k = c; k < 9; ++k) M[r][k] -= f * M[c][k];
    }
  }
  double Hn[9];
  for (int i = 0; i < 8; ++i) Hn[i] = M[i][8] / M[i][i];
  Hn[8] = 1;
  // undo the normalizations: H = Tb^-1 Hn Ta
  const double TaM[9] = {Ta[0], 0, Ta[1], 0, Ta[0], Ta[2], 0, 0, 1};
  const double TbI[9] = {1 / Tb[0], 0, -Tb[1] / Tb[0], 0, 1 / Tb[0], -Tb[2] / Tb[0], 0, 0, 1};
  double T1[9];
  for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) { T1[r * 3 + c] = 0; for (int k = 0; k < 3; ++k) T1[r * 3 + c] += Hn[r * 3 + k] * TaM[k * 3 + c]; }
  for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) { H[r * 3 + c] = 0; for (int k = 0; k < 3; ++k) H[r * 3 + c] += TbI[r * 3 + k] * T1[k * 3 + c]; }
  for (int i = 0; i < 9; ++i) H[i] /= H[8];
  return true;
}

// the focal length a plane's homography implies (the principal point at cx, cy; square pixels): from the two
// orthonormality conditions on its first two columns, each where it is well conditioned; 0 when neither is
double focal_of(const double H0[9], double cx, double cy) {
  double H[9];
  // move the principal point to the origin
  for (int c = 0; c < 3; ++c) { H[c] = H0[c] - cx * H0[6 + c]; H[3 + c] = H0[3 + c] - cy * H0[6 + c]; H[6 + c] = H0[6 + c]; }
  double num = 0, den = 0;
  const double d1 = H[6] * H[7], n1 = -(H[0] * H[1] + H[3] * H[4]);
  const double d2 = H[6] * H[6] - H[7] * H[7], n2 = -(H[0] * H[0] + H[3] * H[3] - H[1] * H[1] - H[4] * H[4]);
  // each condition says f^2 * d = n: combined by least squares
  num = n1 * d1 + n2 * d2;
  den = d1 * d1 + d2 * d2;
  if (den <= 1e-30) return 0;
  const double f2 = num / den;
  return f2 > 0 ? std::sqrt(f2) : 0;
}

// a camera's pose from the homography of a plane (z = 0) and its focal length
bool pose_from_h(const double H[9], Cam& c) {
  // K^-1 H
  double A[9];
  for (int k = 0; k < 3; ++k) {
    A[k] = (H[k] - c.cx * H[6 + k]) / c.f;
    A[3 + k] = (H[3 + k] - c.cy * H[6 + k]) / c.f;
    A[6 + k] = H[6 + k];
  }
  const double l1 = std::sqrt(A[0] * A[0] + A[3] * A[3] + A[6] * A[6]), l2 = std::sqrt(A[1] * A[1] + A[4] * A[4] + A[7] * A[7]);
  if (l1 < 1e-12 || l2 < 1e-12) return false;
  double lam = 2.0 / (l1 + l2);
  if (A[8] * lam < 0) lam = -lam;   // the plane in front of the camera
  double r1[3] = {A[0] * lam, A[3] * lam, A[6] * lam}, r2[3] = {A[1] * lam, A[4] * lam, A[7] * lam};
  // made orthonormal: the two columns' bisector and its perpendicular in their plane
  auto nrm = [](double* v) { const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); for (int i = 0; i < 3; ++i) v[i] /= l; };
  double r3[3] = {r1[1] * r2[2] - r1[2] * r2[1], r1[2] * r2[0] - r1[0] * r2[2], r1[0] * r2[1] - r1[1] * r2[0]};
  nrm(r3);
  double a[3] = {r1[0] + r2[0], r1[1] + r2[1], r1[2] + r2[2]};
  nrm(a);
  double b[3] = {r3[1] * a[2] - r3[2] * a[1], r3[2] * a[0] - r3[0] * a[2], r3[0] * a[1] - r3[1] * a[0]};
  nrm(b);
  for (int i = 0; i < 3; ++i) { r1[i] = (a[i] - b[i]) / std::sqrt(2.0); r2[i] = (a[i] + b[i]) / std::sqrt(2.0); }
  for (int i = 0; i < 3; ++i) { c.R[i * 3] = r1[i]; c.R[i * 3 + 1] = r2[i]; c.R[i * 3 + 2] = r3[i]; c.t[i] = A[i * 3 + 2] * lam; }
  return true;
}

// reprojection refined by Levenberg-Marquardt over rotation, translation and (unless fixed) the focal length
double refine_camera(Cam& c, const std::vector<std::array<double, 3>>& X, const std::vector<P2>& u, bool free_f) {
  const int np = free_f ? 7 : 6;
  auto params = [&](double* p) { rodrigues_of(c.R, p); p[3] = c.t[0]; p[4] = c.t[1]; p[5] = c.t[2]; if (free_f) p[6] = c.f; };
  auto apply = [&](const double* p, Cam& o) { o = c; rodrigues(p, o.R); o.t[0] = p[3]; o.t[1] = p[4]; o.t[2] = p[5]; if (free_f) o.f = p[6]; };
  auto resid = [&](const Cam& o, std::vector<double>& r) {
    r.resize(u.size() * 2);
    double e = 0;
    for (size_t i = 0; i < u.size(); ++i) {
      P2 q;
      if (!project(o, X[i].data(), &q)) q = {1e6, 1e6};
      r[i * 2] = q.x - u[i].x; r[i * 2 + 1] = q.y - u[i].y;
      e += r[i * 2] * r[i * 2] + r[i * 2 + 1] * r[i * 2 + 1];
    }
    return e;
  };
  double p[7];
  params(p);
  std::vector<double> r, r2;
  Cam cur;
  apply(p, cur);
  double e = resid(cur, r), lam = 1e-3;
  for (int it = 0; it < 60; ++it) {
    // the Jacobian by central differences
    std::vector<std::vector<double>> J(np, std::vector<double>(r.size()));
    for (int k = 0; k < np; ++k) {
      const double h = k < 3 ? 1e-6 : (k < 6 ? 1e-4 * std::max(1.0, std::fabs(p[k])) : 1e-3 * std::max(1.0, p[k]));
      double pp[7], pm[7];
      std::memcpy(pp, p, sizeof pp); std::memcpy(pm, p, sizeof pm);
      pp[k] += h; pm[k] -= h;
      Cam a, b;
      apply(pp, a); apply(pm, b);
      std::vector<double> ra, rb;
      resid(a, ra); resid(b, rb);
      for (size_t i = 0; i < r.size(); ++i) J[k][i] = (ra[i] - rb[i]) / (2 * h);
    }
    double A[7][8];
    for (int a = 0; a < np; ++a) {
      for (int b = 0; b < np; ++b) { A[a][b] = 0; for (size_t i = 0; i < r.size(); ++i) A[a][b] += J[a][i] * J[b][i]; }
      A[a][np] = 0;
      for (size_t i = 0; i < r.size(); ++i) A[a][np] -= J[a][i] * r[i];
    }
    bool improved = false;
    for (int tries = 0; tries < 8 && !improved; ++tries) {
      double M[7][8];
      for (int a = 0; a < np; ++a) { for (int b = 0; b <= np; ++b) M[a][b] = A[a][b]; M[a][a] *= 1 + lam; }
      bool ok = true;
      for (int col = 0; col < np && ok; ++col) {
        int piv = col;
        for (int rr = col + 1; rr < np; ++rr) if (std::fabs(M[rr][col]) > std::fabs(M[piv][col])) piv = rr;
        if (std::fabs(M[piv][col]) < 1e-18) { ok = false; break; }
        if (piv != col) for (int k = 0; k <= np; ++k) std::swap(M[col][k], M[piv][k]);
        for (int rr = 0; rr < np; ++rr) {
          if (rr == col) continue;
          const double f = M[rr][col] / M[col][col];
          for (int k = col; k <= np; ++k) M[rr][k] -= f * M[col][k];
        }
      }
      if (!ok) { lam *= 10; continue; }
      double pn[7];
      for (int a = 0; a < np; ++a) pn[a] = p[a] + M[a][np] / M[a][a];
      if (free_f && pn[6] <= 1) { lam *= 10; continue; }
      Cam o;
      apply(pn, o);
      const double en = resid(o, r2);
      if (en < e) { std::memcpy(p, pn, sizeof pn); e = en; r = r2; lam = std::max(1e-9, lam / 10); improved = true; }
      else lam *= 10;
    }
    if (!improved) break;
  }
  apply(p, c);
  return std::sqrt(e / std::max<size_t>(1, u.size()));
}

bool read_cam(const Value& v, Cam& c, std::string& error, const char* who) {
  MapObj* m = v.type == Type::Map ? static_cast<MapObj*>(v.obj.get()) : nullptr;
  auto num = [&](const char* k, double* out) { const Value* x = m ? m->find(k) : nullptr; if (!x || !x->is_number()) return false; *out = x->as_double(); return true; };
  auto arr = [&](const char* k, double* out, size_t n) {
    const Value* x = m ? m->find(k) : nullptr;
    if (!x || x->type != Type::List || x->list_items().size() != n) return false;
    for (size_t i = 0; i < n; ++i) { if (!x->list_items()[i].is_number()) return false; out[i] = x->list_items()[i].as_double(); }
    return true;
  };
  if (!num("focal", &c.f) || !num("cx", &c.cx) || !num("cy", &c.cy) || !arr("rot", c.R, 9) || !arr("t", c.t, 3)) {
    error = std::string(who) + "() wants a camera: {focal, cx, cy, rot (9 numbers), t (3)} as camera_pose gives";
    return false;
  }
  return true;
}
Value cam_value(const Cam& c, double rms) {
  auto m = std::make_shared<MapObj>();
  m->put("focal", Value::real(c.f));
  m->put("cx", Value::real(c.cx));
  m->put("cy", Value::real(c.cy));
  std::vector<Value> R, t, pos;
  for (double v : c.R) R.push_back(Value::real(v));
  for (double v : c.t) t.push_back(Value::real(v));
  // where the camera is in the world: -R^T t
  for (int i = 0; i < 3; ++i) pos.push_back(Value::real(-(c.R[i] * c.t[0] + c.R[3 + i] * c.t[1] + c.R[6 + i] * c.t[2])));
  m->put("rot", Value::list(std::move(R)));
  m->put("t", Value::list(std::move(t)));
  m->put("pos", Value::list(std::move(pos)));
  m->put("rms_px", Value::real(rms));
  return Value::object(Type::Map, m);
}

// camera_pose(world, picture, w, h[, opts]) -> camera: world points on a plane ([x, y], or [x, y, 0]: z = 0) and
// where they are in a w x h picture; the focal length found from the plane's homography (opts.focal fixes it),
// the pose from that, both refined against every point's reprojection
Value bi_camera_pose(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::List || args[1].type != Type::List) { error = "camera_pose() wants the world points and the picture points as lists"; return Value::nil(); }
  std::vector<P2> wp, ip;
  for (const Value& v : args[0].list_items()) {
    if (v.type != Type::List || v.list_items().size() < 2) { error = "camera_pose() wants each world point as [x, y] (on the plane z = 0)"; return Value::nil(); }
    if (v.list_items().size() > 2 && std::fabs(v.list_items()[2].as_double()) > 1e-9) { error = "camera_pose(): the world points must lie on the plane z = 0"; return Value::nil(); }
    wp.push_back({v.list_items()[0].as_double(), v.list_items()[1].as_double()});
  }
  if (!read_points(args[1], ip, wp.size(), error, "camera_pose")) return Value::nil();
  if (wp.size() < 4) { error = "camera_pose() wants four points or more"; return Value::nil(); }
  if (!want_number(args[2], error, "camera_pose") || !want_number(args[3], error, "camera_pose")) return Value::nil();
  MapObj* o = opts_map(args, 4);
  Cam c;
  c.cx = args[2].as_double() / 2; c.cy = args[3].as_double() / 2;
  double H[9];
  if (!fit_homography(wp, ip, H)) { error = "camera_pose(): the points are degenerate (in a line)"; return Value::nil(); }
  const double fixed = opt_num(o, "focal", 0);
  double f = fixed > 0 ? fixed : focal_of(H, c.cx, c.cy);
  const double dflt = 0.8 * std::max(args[2].as_double(), args[3].as_double());
  // a plane seen square on says nothing about the focal length: a phone's usual, then refined
  if (!(f > 0.2 * dflt && f < 5 * dflt)) f = dflt;
  c.f = f;
  if (!pose_from_h(H, c)) { error = "camera_pose(): no pose fits the points"; return Value::nil(); }
  std::vector<std::array<double, 3>> X;
  for (const P2& p : wp) X.push_back({p.x, p.y, 0.0});
  const double rms = refine_camera(c, X, ip, fixed <= 0 && wp.size() >= 6);
  return cam_value(c, rms);
}

// camera_project(camera, points) -> [[x, y] | nil ...]: world points [x, y, z] where the camera sees them (nil
// behind it)
Value bi_camera_project(Vm&, std::vector<Value>& args, std::string& error) {
  Cam c;
  if (!read_cam(args[0], c, error, "camera_project")) return Value::nil();
  if (args[1].type != Type::List) { error = "camera_project() wants a list of [x, y, z] points"; return Value::nil(); }
  std::vector<Value> out;
  for (const Value& v : args[1].list_items()) {
    if (v.type != Type::List || v.list_items().size() < 3) { error = "camera_project() wants each point as [x, y, z]"; return Value::nil(); }
    const double X[3] = {v.list_items()[0].as_double(), v.list_items()[1].as_double(), v.list_items()[2].as_double()};
    P2 q;
    if (project(c, X, &q)) out.push_back(Value::list({Value::real(q.x), Value::real(q.y)}));
    else out.push_back(Value::nil());
  }
  return Value::list(std::move(out));
}

// camera_ray(camera, [x, y]) -> {from: [x y z], dir: [x y z]}: the world ray through a picture point
Value bi_camera_ray(Vm&, std::vector<Value>& args, std::string& error) {
  Cam c;
  if (!read_cam(args[0], c, error, "camera_ray")) return Value::nil();
  std::vector<P2> pt;
  if (args[1].type != Type::List || args[1].list_items().size() != 2) { error = "camera_ray() wants a picture point [x, y]"; return Value::nil(); }
  const double u = args[1].list_items()[0].as_double(), v = args[1].list_items()[1].as_double();
  const double d[3] = {(u - c.cx) / c.f, (v - c.cy) / c.f, 1};
  double w[3], from[3];
  for (int i = 0; i < 3; ++i) {
    w[i] = c.R[i] * d[0] + c.R[3 + i] * d[1] + c.R[6 + i] * d[2];
    from[i] = -(c.R[i] * c.t[0] + c.R[3 + i] * c.t[1] + c.R[6 + i] * c.t[2]);
  }
  const double l = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
  auto m = std::make_shared<MapObj>();
  m->put("from", Value::list({Value::real(from[0]), Value::real(from[1]), Value::real(from[2])}));
  m->put("dir", Value::list({Value::real(w[0] / l), Value::real(w[1] / l), Value::real(w[2] / l)}));
  return Value::object(Type::Map, m);
}

// ---- a plane with a known square and a rectangle on it -------------------------------------------------------

// Levenberg-Marquardt over any parameters: residual(params, out) fills the residuals; returns the rms
double lm_solve(std::vector<double>& p, const std::function<void(const std::vector<double>&, std::vector<double>&)>& residual, int iters = 100) {
  std::vector<double> r, ra, rb;
  residual(p, r);
  auto sq = [](const std::vector<double>& v) { double e = 0; for (double x : v) e += x * x; return e; };
  double e = sq(r), lam = 1e-3;
  const size_t np = p.size();
  for (int it = 0; it < iters; ++it) {
    std::vector<std::vector<double>> J(np);
    for (size_t k = 0; k < np; ++k) {
      const double h = 1e-6 * std::max(1.0, std::fabs(p[k]));
      std::vector<double> pp(p), pm(p);
      pp[k] += h; pm[k] -= h;
      residual(pp, ra); residual(pm, rb);
      J[k].resize(r.size());
      for (size_t i = 0; i < r.size(); ++i) J[k][i] = (ra[i] - rb[i]) / (2 * h);
    }
    std::vector<std::vector<double>> A(np, std::vector<double>(np + 1, 0));
    for (size_t a = 0; a < np; ++a) {
      for (size_t b = 0; b < np; ++b) for (size_t i = 0; i < r.size(); ++i) A[a][b] += J[a][i] * J[b][i];
      for (size_t i = 0; i < r.size(); ++i) A[a][np] -= J[a][i] * r[i];
    }
    bool improved = false;
    for (int tries = 0; tries < 10 && !improved; ++tries) {
      auto M = A;
      for (size_t a = 0; a < np; ++a) M[a][a] *= 1 + lam;
      bool ok = true;
      for (size_t c = 0; c < np && ok; ++c) {
        size_t piv = c;
        for (size_t rr = c + 1; rr < np; ++rr) if (std::fabs(M[rr][c]) > std::fabs(M[piv][c])) piv = rr;
        if (std::fabs(M[piv][c]) < 1e-20) { ok = false; break; }
        std::swap(M[c], M[piv]);
        for (size_t rr = 0; rr < np; ++rr) {
          if (rr == c) continue;
          const double f = M[rr][c] / M[c][c];
          for (size_t k = c; k <= np; ++k) M[rr][k] -= f * M[c][k];
        }
      }
      if (!ok) { lam *= 10; continue; }
      std::vector<double> pn(p);
      for (size_t a = 0; a < np; ++a) pn[a] += M[a][np] / M[a][a];
      std::vector<double> rn;
      residual(pn, rn);
      const double en = sq(rn);
      if (std::isfinite(en) && en < e) { p = pn; r = rn; e = en; lam = std::max(1e-12, lam / 10); improved = true; }
      else lam *= 10;
    }
    if (!improved) break;
  }
  return std::sqrt(e / std::max<size_t>(1, r.size() / 2));
}

// camera_pose_rect(marker, marker_px, rect_px, w, h[, opts]) -> {camera, rect: {corners (mm, on the plane), x, y,
//   angle (degrees), width, height}, rms_px}
//   A plane with a square of known size on it (marker: its corners in mm, as camera_pose takes them; marker_px:
//   where the picture has them) and a rectangle of unknown size (rect_px: its corners in the picture, clockwise from
//   the top left). The rectangle's right angles say a great deal about the camera that a small square cannot; the
//   camera (focal length included unless opts.focal fixes it) and the rectangle (where it is, how it is turned, how
//   big) are found together, by least squares on the picture positions of all eight corners.
Value bi_camera_pose_rect(Vm&, std::vector<Value>& args, std::string& error) {
  std::vector<P2> mw, mp, rp;
  if (!read_points(args[0], mw, 0, error, "camera_pose_rect") || !read_points(args[1], mp, mw.size(), error, "camera_pose_rect") ||
      !read_points(args[2], rp, 4, error, "camera_pose_rect")) return Value::nil();
  if (mw.size() < 4) { error = "camera_pose_rect() wants the square's four corners"; return Value::nil(); }
  if (!want_number(args[3], error, "camera_pose_rect") || !want_number(args[4], error, "camera_pose_rect")) return Value::nil();
  MapObj* o = opts_map(args, 5);
  const double W = args[3].as_double(), H = args[4].as_double();
  Cam c;
  c.cx = W / 2; c.cy = H / 2;
  const double fixed = opt_num(o, "focal", 0);
  // a start: the focal length from the rectangle's vanishing points (its edges are two pairs of parallels at right
  // angles), else from the square, else a phone's usual
  double f0 = fixed;
  if (f0 <= 0) {
    auto line = [](P2 a, P2 b) { return std::array<double, 3>{a.y - b.y, b.x - a.x, a.x * b.y - b.x * a.y}; };
    auto meet3 = [](const std::array<double, 3>& l, const std::array<double, 3>& m) {
      return std::array<double, 3>{l[1] * m[2] - l[2] * m[1], l[2] * m[0] - l[0] * m[2], l[0] * m[1] - l[1] * m[0]};
    };
    const auto v1 = meet3(line(rp[0], rp[1]), line(rp[3], rp[2])), v2 = meet3(line(rp[0], rp[3]), line(rp[1], rp[2]));
    const double num = -((v1[0] - c.cx * v1[2]) * (v2[0] - c.cx * v2[2]) + (v1[1] - c.cy * v1[2]) * (v2[1] - c.cy * v2[2]));
    const double den = v1[2] * v2[2];
    if (std::fabs(den) > 1e-12 && num / den > 0) f0 = std::sqrt(num / den);
    const double dflt = 0.8 * std::max(W, H);
    if (!(f0 > 0.2 * dflt && f0 < 5 * dflt)) {
      double Hm[9];
      f0 = fit_homography(mw, mp, Hm) ? focal_of(Hm, c.cx, c.cy) : 0;
      if (!(f0 > 0.2 * dflt && f0 < 5 * dflt)) f0 = dflt;
    }
  }
  c.f = f0;
  double Hm[9];
  if (!fit_homography(mw, mp, Hm) || !pose_from_h(Hm, c)) { error = "camera_pose_rect(): no pose fits the square"; return Value::nil(); }
  // the rectangle's start: its corners on the plane by the square's homography
  double Hi[9];
  {
    std::vector<P2> pw(mw), pp(mp);
    if (!fit_homography(pp, pw, Hi)) { error = "camera_pose_rect(): the square is degenerate"; return Value::nil(); }
  }
  P2 q[4];
  for (int i = 0; i < 4; ++i) q[i] = apply_h(Hi, rp[(size_t)i]);
  const double ang0 = std::atan2(q[2].y - q[3].y, q[2].x - q[3].x);
  const double w0 = (std::hypot(q[1].x - q[0].x, q[1].y - q[0].y) + std::hypot(q[2].x - q[3].x, q[2].y - q[3].y)) / 2;
  const double h0 = (std::hypot(q[3].x - q[0].x, q[3].y - q[0].y) + std::hypot(q[2].x - q[1].x, q[2].y - q[1].y)) / 2;
  // parameters: rotation (3), translation (3), rectangle (bottom-left x, y, angle, width, height), focal (unless fixed)
  std::vector<double> p(11 + (fixed > 0 ? 0 : 1));
  double rv[3];
  rodrigues_of(c.R, rv);
  p[0] = rv[0]; p[1] = rv[1]; p[2] = rv[2]; p[3] = c.t[0]; p[4] = c.t[1]; p[5] = c.t[2];
  p[6] = q[3].x; p[7] = q[3].y; p[8] = ang0; p[9] = w0; p[10] = h0;
  if (fixed <= 0) p[11] = c.f;
  auto unpack = [&](const std::vector<double>& v, Cam& o2, P2 rc[4]) {
    o2 = c;
    rodrigues(v.data(), o2.R);
    o2.t[0] = v[3]; o2.t[1] = v[4]; o2.t[2] = v[5];
    if (fixed <= 0) o2.f = v[11];
    const double ux = std::cos(v[8]), uy = std::sin(v[8]);
    const P2 bl{v[6], v[7]};
    rc[3] = bl;
    rc[2] = {bl.x + ux * v[9], bl.y + uy * v[9]};
    rc[0] = {bl.x - uy * v[10], bl.y + ux * v[10]};
    rc[1] = {rc[2].x - uy * v[10], rc[2].y + ux * v[10]};
  };
  auto residual = [&](const std::vector<double>& v, std::vector<double>& r) {
    Cam o2;
    P2 rc[4];
    unpack(v, o2, rc);
    r.clear();
    auto add = [&](P2 wpt, P2 obs) {
      const double X[3] = {wpt.x, wpt.y, 0};
      P2 q2;
      if (!project(o2, X, &q2)) q2 = {1e6, 1e6};
      r.push_back(q2.x - obs.x); r.push_back(q2.y - obs.y);
    };
    for (size_t i = 0; i < mw.size(); ++i) add(mw[i], mp[i]);
    for (int i = 0; i < 4; ++i) add(rc[i], rp[(size_t)i]);
  };
  const double rms = lm_solve(p, residual);
  Cam out;
  P2 rc[4];
  unpack(p, out, rc);
  auto rect = std::make_shared<MapObj>();
  rect->put("corners", points_value(std::vector<P2>(rc, rc + 4)));
  rect->put("x", Value::real(p[6]));
  rect->put("y", Value::real(p[7]));
  rect->put("angle", Value::real(p[8] * 180.0 / M_PI));
  rect->put("width", Value::real(p[9]));
  rect->put("height", Value::real(p[10]));
  auto res = std::make_shared<MapObj>();
  res->put("camera", cam_value(out, rms));
  res->put("rect", Value::object(Type::Map, rect));
  res->put("rms_px", Value::real(rms));
  return Value::object(Type::Map, res);
}

// box_face_views(photo, camera, ref, ref_camera, box, mm_per_px) -> {actual, expected, w, h, known}
//   A box open on its front (its front face the plane z = 0 from (0, 0) to (box.w, box.h) in the cameras' world,
//   its inside reaching back to z = -box.d, away from the cameras) seen in two photos: ref, when it was empty, and
//   photo, now. actual: the front face as photo shows it, flattened at mm_per_px; expected: the same view of the
//   empty box made from ref - each pixel's ray followed inside to the wall, floor or back it meets, and that point
//   taken from ref where ref sees it (transparent where ref's view of it is cut off by the front). Their difference
//   is what is in the box now and was not, from wherever each photo was taken.
Value bi_box_face_views(Vm&, std::vector<Value>& args, std::string& error) {
  uint32_t W = 0, H = 0, RW = 0, RH = 0;
  const std::vector<uint32_t>* px = nullptr;
  const std::vector<uint32_t>* rpx = nullptr;
  Cam c, rc;
  if (!canvas_pixels(args[0], &W, &H, &px, error, "box_face_views") || !read_cam(args[1], c, error, "box_face_views") ||
      !canvas_pixels(args[2], &RW, &RH, &rpx, error, "box_face_views") || !read_cam(args[3], rc, error, "box_face_views")) return Value::nil();
  MapObj* b = args[4].type == Type::Map ? static_cast<MapObj*>(args[4].obj.get()) : nullptr;
  const double bw = opt_num(b, "w", 0), bh = opt_num(b, "h", 0), bd = opt_num(b, "d", 0);
  if (!(bw > 0 && bh > 0 && bd > 0)) { error = "box_face_views() wants the box as {w, h, d} (mm, each more than 0)"; return Value::nil(); }
  if (!want_number(args[5], error, "box_face_views")) return Value::nil();
  const double mpp = args[5].as_double();
  if (!(mpp > 0)) { error = "box_face_views(): mm_per_px is more than 0"; return Value::nil(); }
  const int ow = std::max(1, (int)std::lround(bw / mpp)), oh = std::max(1, (int)std::lround(bh / mpp));
  if ((size_t)ow * oh > 16u * 1024 * 1024) { error = "box_face_views(): that face is too many pixels at that scale"; return Value::nil(); }
  auto sample = [](const std::vector<uint32_t>& im, uint32_t iw, uint32_t ih, P2 q) -> uint32_t {
    const double fx = q.x - 0.5, fy = q.y - 0.5;
    if (fx < 0 || fy < 0 || fx > iw - 1 || fy > ih - 1) return 0;
    const int x0 = (int)fx, y0 = (int)fy, x1 = std::min<int>(x0 + 1, iw - 1), y1 = std::min<int>(y0 + 1, ih - 1);
    const double tx = fx - x0, ty = fy - y0;
    uint32_t out = 0xFF000000u;
    for (int sh = 0; sh < 24; sh += 8) {
      auto g = [&](int xx, int yy) { return (double)((im[(size_t)yy * iw + xx] >> sh) & 255); };
      const double v = (g(x0, y0) * (1 - tx) + g(x1, y0) * tx) * (1 - ty) + (g(x0, y1) * (1 - tx) + g(x1, y1) * tx) * ty;
      out |= (uint32_t)std::clamp(std::lround(v), 0L, 255L) << sh;
    }
    return out;
  };
  double from[3];
  for (int i = 0; i < 3; ++i) from[i] = -(c.R[i] * c.t[0] + c.R[3 + i] * c.t[1] + c.R[6 + i] * c.t[2]);
  double rfrom[3];
  for (int i = 0; i < 3; ++i) rfrom[i] = -(rc.R[i] * rc.t[0] + rc.R[3 + i] * rc.t[1] + rc.R[6 + i] * rc.t[2]);
  std::vector<uint32_t> act((size_t)ow * oh, 0), exp((size_t)ow * oh, 0);
  size_t known = 0;
  for (int y = 0; y < oh; ++y)
    for (int x = 0; x < ow; ++x) {
      // the face point (top row first: the face's top is y = h)
      const double F[3] = {(x + 0.5) * mpp, bh - (y + 0.5) * mpp, 0};
      P2 q;
      if (!project(c, F, &q)) continue;
      act[(size_t)y * ow + x] = sample(*px, W, H, q);
      // into the box: from the face point along the ray away from the camera, to where it leaves the box
      double d[3] = {F[0] - from[0], F[1] - from[1], F[2] - from[2]};
      if (d[2] >= 0) continue;   // the camera behind the face
      double t = (-bd - F[2]) / d[2];   // the back
      if (d[0] > 0) t = std::min(t, (bw - F[0]) / d[0]); else if (d[0] < 0) t = std::min(t, (0 - F[0]) / d[0]);
      if (d[1] > 0) t = std::min(t, (bh - F[1]) / d[1]); else if (d[1] < 0) t = std::min(t, (0 - F[1]) / d[1]);
      const double X[3] = {F[0] + d[0] * t, F[1] + d[1] * t, F[2] + d[2] * t};
      // ref sees it only through the opening: where ref's ray to it crosses the face
      const double e[3] = {X[0] - rfrom[0], X[1] - rfrom[1], X[2] - rfrom[2]};
      if (std::fabs(e[2]) < 1e-12) continue;
      const double s = -rfrom[2] / e[2];
      const double cxp = rfrom[0] + e[0] * s, cyp = rfrom[1] + e[1] * s;
      if (cxp < -0.5 * mpp || cyp < -0.5 * mpp || cxp > bw + 0.5 * mpp || cyp > bh + 0.5 * mpp) continue;
      P2 rq;
      if (!project(rc, X, &rq)) continue;
      const uint32_t v = sample(*rpx, RW, RH, rq);
      if (!v) continue;
      exp[(size_t)y * ow + x] = v;
      ++known;
    }
  auto res = std::make_shared<MapObj>();
  res->put("actual", canvas_value((uint32_t)ow, (uint32_t)oh, std::move(act)));
  res->put("expected", canvas_value((uint32_t)ow, (uint32_t)oh, std::move(exp)));
  res->put("w", Value::integer(ow));
  res->put("h", Value::integer(oh));
  res->put("known", Value::real((double)known / ((double)ow * oh)));
  return Value::object(Type::Map, res);
}

// ---- reconstruction: silhouettes carved into a solid ----------------------------------------------------------

struct Box3 { double lo[3], hi[3]; };
bool read_box(const Value& v, Box3& b, std::string& error, const char* who) {
  MapObj* m = v.type == Type::Map ? static_cast<MapObj*>(v.obj.get()) : nullptr;
  const Value* lo = m ? m->find("min") : nullptr;
  const Value* hi = m ? m->find("max") : nullptr;
  if (!lo || !hi || lo->type != Type::List || hi->type != Type::List || lo->list_items().size() != 3 || hi->list_items().size() != 3) {
    error = std::string(who) + "() wants a box: {min: [x, y, z], max: [x, y, z]}";
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    b.lo[i] = lo->list_items()[(size_t)i].as_double(); b.hi[i] = hi->list_items()[(size_t)i].as_double();
    if (!(b.hi[i] > b.lo[i])) { error = std::string(who) + "(): the box's max must be past its min on every axis"; return false; }
  }
  return true;
}

// camera_box_mask(camera, w, h, box) -> bytes: 255 where the pixel's ray misses the box (certainly not what stands
// in it), 0 where it passes through - image_cutout's background_mask for a photo of something inside the box
Value bi_camera_box_mask(Vm&, std::vector<Value>& args, std::string& error) {
  Cam c;
  if (!read_cam(args[0], c, error, "camera_box_mask")) return Value::nil();
  if (!want_number(args[1], error, "camera_box_mask") || !want_number(args[2], error, "camera_box_mask")) return Value::nil();
  const int64_t w = args[1].as_int(), h = args[2].as_int();
  if (w < 1 || h < 1 || w > 16384 || h > 16384) { error = "camera_box_mask(): the size is 1..16384 each way"; return Value::nil(); }
  Box3 b;
  if (!read_box(args[3], b, error, "camera_box_mask")) return Value::nil();
  double from[3];
  for (int i = 0; i < 3; ++i) from[i] = -(c.R[i] * c.t[0] + c.R[3 + i] * c.t[1] + c.R[6 + i] * c.t[2]);
  std::vector<uint8_t> out((size_t)w * h, 0);
  for (int64_t y = 0; y < h; ++y)
    for (int64_t x = 0; x < w; ++x) {
      const double d[3] = {(x + 0.5 - c.cx) / c.f, (y + 0.5 - c.cy) / c.f, 1};
      double dir[3];
      for (int i = 0; i < 3; ++i) dir[i] = c.R[i] * d[0] + c.R[3 + i] * d[1] + c.R[6 + i] * d[2];
      // the slab test
      double t0 = 0, t1 = 1e30;
      for (int i = 0; i < 3 && t0 <= t1; ++i) {
        if (std::fabs(dir[i]) < 1e-15) { if (from[i] < b.lo[i] || from[i] > b.hi[i]) t0 = 2e30; continue; }
        double a = (b.lo[i] - from[i]) / dir[i], z = (b.hi[i] - from[i]) / dir[i];
        if (a > z) std::swap(a, z);
        t0 = std::max(t0, a); t1 = std::min(t1, z);
      }
      if (t0 > t1) out[(size_t)y * w + x] = 255;
    }
  return Value::bytes(std::move(out));
}

// visual_hull(views, box[, opts]) -> {positions, normals, colors, indices, vertex_count, tris, volume_mm3, voxels,
//   cell_mm, min, max}
//   views: [{mask (bytes, inside where >= 128), w, h, camera, image (a canvas, optional: for colour)}]; box: where
//   the thing is, {min, max} in world units. Every cell of a grid over the box that some photo shows outside its
//   silhouette is cut away (opts.misses of them forgiven, for a bad cut); a cell no more than half the photos see
//   at all is cut too; what is left is the largest piece. Its surface (surface nets over the occupancy, smoothed)
//   is a closed mesh in world units; each vertex is coloured from the photos that see it (a depth test against the
//   mesh), weighted toward the one facing it squarest; one no photo sees (its underside) takes its neighbours'.
//   coloured: how many vertices a photo saw. opts: {res (cells along the box's longest side; 160),
//   misses (0), smooth (passes; 1)}.
Value bi_visual_hull(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::List || args[0].list_items().empty()) { error = "visual_hull() wants a list of views"; return Value::nil(); }
  Box3 box;
  if (!read_box(args[1], box, error, "visual_hull")) return Value::nil();
  MapObj* o = opts_map(args, 2);
  const int res = (int)std::clamp(opt_num(o, "res", 160), 16.0, 512.0);
  const int misses = (int)std::clamp(opt_num(o, "misses", 0), 0.0, 100.0);
  const int smooth = (int)std::clamp(opt_num(o, "smooth", 1), 0.0, 8.0);
  struct View { Cam cam; int w = 0, h = 0; const std::vector<uint8_t>* mask = nullptr; uint32_t iw = 0, ih = 0; const std::vector<uint32_t>* img = nullptr; };
  std::vector<View> vs;
  for (const Value& v : args[0].list_items()) {
    MapObj* m = v.type == Type::Map ? static_cast<MapObj*>(v.obj.get()) : nullptr;
    if (!m) { error = "visual_hull(): each view is a map {mask, w, h, camera, image}"; return Value::nil(); }
    View vw;
    const Value* cam = m->find("camera");
    if (!cam || !read_cam(*cam, vw.cam, error, "visual_hull")) return Value::nil();
    const Value* wv = m->find("w"); const Value* hv = m->find("h"); const Value* mk = m->find("mask");
    if (!wv || !hv || !mk || !wv->is_number() || !hv->is_number() || mk->type != Type::Bytes) { error = "visual_hull(): each view needs mask (bytes), w and h"; return Value::nil(); }
    vw.w = (int)wv->as_int(); vw.h = (int)hv->as_int(); vw.mask = &mk->bytes_data();
    if ((size_t)vw.w * vw.h != vw.mask->size()) { error = "visual_hull(): a view's mask is not w*h bytes"; return Value::nil(); }
    if (const Value* im = m->find("image"); im && im->type != Type::Nil) {
      if (!canvas_pixels(*im, &vw.iw, &vw.ih, &vw.img, error, "visual_hull")) return Value::nil();
    }
    vs.push_back(vw);
  }
  // the grid: cells of side s, one empty cell of padding all round
  double ext = 0;
  for (int i = 0; i < 3; ++i) ext = std::max(ext, box.hi[i] - box.lo[i]);
  const double s = ext / res;
  int n[3];
  for (int i = 0; i < 3; ++i) n[i] = std::max(1, (int)std::ceil((box.hi[i] - box.lo[i]) / s)) + 2;
  const int NX = n[0], NY = n[1], NZ = n[2];
  const size_t N = (size_t)NX * NY * NZ;
  auto at = [&](int x, int y, int z) { return ((size_t)z * NY + y) * NX + x; };
  auto centre = [&](int x, int y, int z, double* X) { X[0] = box.lo[0] + (x - 0.5) * s; X[1] = box.lo[1] + (y - 0.5) * s; X[2] = box.lo[2] + (z - 0.5) * s; };
  std::vector<uint8_t> occ(N, 0);
  const int need_seen = std::max(1, (int)(vs.size() + 1) / 2);
  for (int z = 1; z < NZ - 1; ++z)
    for (int y = 1; y < NY - 1; ++y)
      for (int x = 1; x < NX - 1; ++x) {
        double X[3];
        centre(x, y, z, X);
        int miss = 0, seen = 0;
        for (const View& v : vs) {
          P2 q;
          if (!project(v.cam, X, &q)) continue;
          const int px = (int)std::floor(q.x), py = (int)std::floor(q.y);
          if (px < 0 || py < 0 || px >= v.w || py >= v.h) continue;
          ++seen;
          if ((*v.mask)[(size_t)py * v.w + px] < 128 && ++miss > misses) break;
        }
        if (miss <= misses && seen >= need_seen) occ[at(x, y, z)] = 1;
      }
  // the largest piece (six-connected)
  {
    std::vector<int> lab(N, -1);
    std::vector<size_t> sizes;
    std::vector<size_t> st;
    for (size_t i = 0; i < N; ++i) {
      if (!occ[i] || lab[i] >= 0) continue;
      const int id = (int)sizes.size();
      size_t cnt = 0;
      lab[i] = id;
      st.push_back(i);
      while (!st.empty()) {
        const size_t p = st.back();
        st.pop_back();
        ++cnt;
        const int x = (int)(p % NX), y = (int)((p / NX) % NY), z = (int)(p / ((size_t)NX * NY));
        const int nb[6][3] = {{x - 1, y, z}, {x + 1, y, z}, {x, y - 1, z}, {x, y + 1, z}, {x, y, z - 1}, {x, y, z + 1}};
        for (const auto& q : nb) {
          if (q[0] < 0 || q[1] < 0 || q[2] < 0 || q[0] >= NX || q[1] >= NY || q[2] >= NZ) continue;
          const size_t k = at(q[0], q[1], q[2]);
          if (occ[k] && lab[k] < 0) { lab[k] = id; st.push_back(k); }
        }
      }
      sizes.push_back(cnt);
    }
    if (sizes.empty()) { error = "visual_hull(): the silhouettes leave nothing in the box (are the cameras and the box right?)"; return Value::nil(); }
    const int big = (int)(std::max_element(sizes.begin(), sizes.end()) - sizes.begin());
    for (size_t i = 0; i < N; ++i) occ[i] = lab[i] == big;
  }
  size_t voxels = 0;
  for (uint8_t v : occ) voxels += v;
  // the field: occupancy, box-blurred along each axis
  std::vector<float> F(N);
  for (size_t i = 0; i < N; ++i) F[i] = occ[i];
  for (int pass = 0; pass < smooth; ++pass)
    for (int axis = 0; axis < 3; ++axis) {
      std::vector<float> G(F);
      const int step[3] = {1, NX, NX * NY};
      for (int z = 1; z < NZ - 1; ++z)
        for (int y = 1; y < NY - 1; ++y)
          for (int x = 1; x < NX - 1; ++x) {
            const size_t i = at(x, y, z);
            G[i] = (F[i - step[axis]] + F[i] + F[i + step[axis]]) / 3.0f;
          }
      F.swap(G);
    }
  const int corner[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
  // no two solid samples touching only along an edge or at a corner (each makes an edge four faces share): the
  // first empty sample of such a pair's square or cube filled, until there are none
  for (int pass = 0; pass < 16; ++pass) {
    size_t fixed = 0;
    auto in = [&](int x, int y, int z) { return F[at(x, y, z)] > 0.5f; };
    auto fill = [&](int x, int y, int z) { F[at(x, y, z)] = 0.51f; ++fixed; };
    for (int z = 0; z + 1 < NZ; ++z)
      for (int y = 0; y + 1 < NY; ++y)
        for (int x = 0; x + 1 < NX; ++x) {
          // the three squares at this sample (xy, xz, yz)
          const int sq[3][4][3] = {{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}}, {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}, {1, 0, 1}}, {{0, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 1, 1}}};
          for (const auto& q : sq) {
            const bool a = in(x + q[0][0], y + q[0][1], z + q[0][2]), b = in(x + q[1][0], y + q[1][1], z + q[1][2]);
            const bool c = in(x + q[2][0], y + q[2][1], z + q[2][2]), d = in(x + q[3][0], y + q[3][1], z + q[3][2]);
            if (a && d && !b && !c) fill(x + q[1][0], y + q[1][1], z + q[1][2]);
            else if (b && c && !a && !d) fill(x + q[0][0], y + q[0][1], z + q[0][2]);
          }
          // the cube: two opposite corners solid and nothing else
          int cnt = 0;
          for (int k = 0; k < 8; ++k) cnt += in(x + corner[k][0], y + corner[k][1], z + corner[k][2]);
          if (cnt == 2)
            for (int k = 0; k < 4; ++k)
              if (in(x + corner[k][0], y + corner[k][1], z + corner[k][2]) && in(x + corner[7 - k][0], y + corner[7 - k][1], z + corner[7 - k][2])) {
                const int j = k == 0 ? 1 : 0;
                fill(x + corner[j][0], y + corner[j][1], z + corner[j][2]);
                break;
              }
        }
    if (fixed == 0) break;
  }
  // surface nets: a vertex in each cube of eight samples the 0.5 level passes through, at the mean of where it
  // crosses the cube's edges; a quad for each sample edge it crosses, joining the four cubes round that edge
  std::vector<int> vid((size_t)(NX - 1) * (NY - 1) * (NZ - 1), -1);
  auto cube = [&](int x, int y, int z) { return ((size_t)z * (NY - 1) + y) * (NX - 1) + x; };
  std::vector<double> P;   // world positions
  const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  for (int z = 0; z < NZ - 1; ++z)
    for (int y = 0; y < NY - 1; ++y)
      for (int x = 0; x < NX - 1; ++x) {
        float v[8];
        int inside = 0;
        for (int k = 0; k < 8; ++k) { v[k] = F[at(x + corner[k][0], y + corner[k][1], z + corner[k][2])] - 0.5f; inside += v[k] > 0; }
        if (inside == 0 || inside == 8) continue;
        double sx = 0, sy = 0, sz = 0;
        int cnt = 0;
        for (const auto& e : edges) {
          const float a = v[e[0]], b = v[e[1]];
          if ((a > 0) == (b > 0)) continue;
          const double t = a / (a - b);
          sx += corner[e[0]][0] + t * (corner[e[1]][0] - corner[e[0]][0]);
          sy += corner[e[0]][1] + t * (corner[e[1]][1] - corner[e[0]][1]);
          sz += corner[e[0]][2] + t * (corner[e[1]][2] - corner[e[0]][2]);
          ++cnt;
        }
        vid[cube(x, y, z)] = (int)(P.size() / 3);
        // sample (i, j, k) sits at the centre of cell (i, j, k): box.lo + (i - 0.5) s
        P.push_back(box.lo[0] + (x + sx / cnt - 0.5) * s);
        P.push_back(box.lo[1] + (y + sy / cnt - 0.5) * s);
        P.push_back(box.lo[2] + (z + sz / cnt - 0.5) * s);
      }
  const size_t nv = P.size() / 3;
  // the field's gradient at a vertex (trilinear, central differences): its outward normal is minus it
  auto field = [&](double gx, double gy, double gz) {
    gx = std::clamp(gx, 0.0, NX - 1.001); gy = std::clamp(gy, 0.0, NY - 1.001); gz = std::clamp(gz, 0.0, NZ - 1.001);
    const int x = (int)gx, y = (int)gy, z = (int)gz;
    const double tx = gx - x, ty = gy - y, tz = gz - z;
    double r = 0;
    for (int k = 0; k < 8; ++k) {
      const double w = (corner[k][0] ? tx : 1 - tx) * (corner[k][1] ? ty : 1 - ty) * (corner[k][2] ? tz : 1 - tz);
      r += w * F[at(std::min(NX - 1, x + corner[k][0]), std::min(NY - 1, y + corner[k][1]), std::min(NZ - 1, z + corner[k][2]))];
    }
    return r;
  };
  std::vector<double> Nn(nv * 3);
  for (size_t i = 0; i < nv; ++i) {
    const double gx = (P[i * 3] - box.lo[0]) / s + 0.5, gy = (P[i * 3 + 1] - box.lo[1]) / s + 0.5, gz = (P[i * 3 + 2] - box.lo[2]) / s + 0.5;
    double d[3] = {field(gx + 0.5, gy, gz) - field(gx - 0.5, gy, gz), field(gx, gy + 0.5, gz) - field(gx, gy - 0.5, gz), field(gx, gy, gz + 0.5) - field(gx, gy, gz - 0.5)};
    const double l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int k = 0; k < 3; ++k) Nn[i * 3 + k] = l > 0 ? -d[k] / l : 0;
  }
  std::vector<uint32_t> I;
  auto emit = [&](int a, int b, int c, int d) {
    if (a < 0 || b < 0 || c < 0 || d < 0) return;
    // wound so the face turns the way the normals do (outward)
    const double* pa = &P[(size_t)a * 3]; const double* pb = &P[(size_t)b * 3]; const double* pc = &P[(size_t)c * 3];
    const double u[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]}, w[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
    const double fn[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
    double nsum[3] = {0, 0, 0};
    for (int q : {a, b, c, d}) for (int k = 0; k < 3; ++k) nsum[k] += Nn[(size_t)q * 3 + k];
    if (fn[0] * nsum[0] + fn[1] * nsum[1] + fn[2] * nsum[2] < 0) { std::swap(b, d); }
    I.insert(I.end(), {(uint32_t)a, (uint32_t)b, (uint32_t)c, (uint32_t)a, (uint32_t)c, (uint32_t)d});
  };
  // (from the padding layer in: a thing standing on the box's floor has its bottom on the edges from layer 0)
  for (int z = 0; z < NZ; ++z)
    for (int y = 0; y < NY; ++y)
      for (int x = 0; x < NX; ++x) {
        const bool here = F[at(x, y, z)] > 0.5f;
        const bool yz = y >= 1 && z >= 1 && y < NY - 1 && z < NZ - 1, xz = x >= 1 && z >= 1 && x < NX - 1 && z < NZ - 1,
                   xy = x >= 1 && y >= 1 && x < NX - 1 && y < NY - 1;
        // the edge to the next sample along x: the four cubes round it share (y-1..y, z-1..z)
        if (yz && x + 1 < NX && here != (F[at(x + 1, y, z)] > 0.5f))
          emit(vid[cube(x, y - 1, z - 1)], vid[cube(x, y, z - 1)], vid[cube(x, y, z)], vid[cube(x, y - 1, z)]);
        if (xz && y + 1 < NY && here != (F[at(x, y + 1, z)] > 0.5f))
          emit(vid[cube(x - 1, y, z - 1)], vid[cube(x, y, z - 1)], vid[cube(x, y, z)], vid[cube(x - 1, y, z)]);
        if (xy && z + 1 < NZ && here != (F[at(x, y, z + 1)] > 0.5f))
          emit(vid[cube(x - 1, y - 1, z)], vid[cube(x, y - 1, z)], vid[cube(x, y, z)], vid[cube(x - 1, y, z)]);
      }
  if (I.empty()) { error = "visual_hull(): what is left is too small to make a surface of"; return Value::nil(); }
  // colour: each view's depth of the mesh (a z-buffer at up to 640 across), then each vertex from the views that
  // see it, by how squarely each faces it
  std::vector<double> C(nv * 3, 0), Wt(nv, 0);
  for (const View& v : vs) {
    if (!v.img) continue;
    const double k = std::min(1.0, 640.0 / v.w);
    const int zw = std::max(1, (int)(v.w * k)), zh = std::max(1, (int)(v.h * k));
    std::vector<float> zb((size_t)zw * zh, 1e30f);
    std::vector<P2> sp(nv);
    std::vector<double> sd(nv);
    std::vector<uint8_t> ok(nv);
    for (size_t i = 0; i < nv; ++i) { ok[i] = project(v.cam, &P[i * 3], &sp[i], &sd[i]); sp[i].x *= k; sp[i].y *= k; }
    for (size_t t = 0; t + 2 < I.size(); t += 3) {
      const uint32_t a = I[t], b = I[t + 1], c = I[t + 2];
      if (!ok[a] || !ok[b] || !ok[c]) continue;
      const double x0 = std::min({sp[a].x, sp[b].x, sp[c].x}), x1 = std::max({sp[a].x, sp[b].x, sp[c].x});
      const double y0 = std::min({sp[a].y, sp[b].y, sp[c].y}), y1 = std::max({sp[a].y, sp[b].y, sp[c].y});
      const double den = (sp[b].y - sp[c].y) * (sp[a].x - sp[c].x) + (sp[c].x - sp[b].x) * (sp[a].y - sp[c].y);
      if (std::fabs(den) < 1e-12) continue;
      for (int py = std::max(0, (int)std::floor(y0)); py <= std::min(zh - 1, (int)std::ceil(y1)); ++py)
        for (int px = std::max(0, (int)std::floor(x0)); px <= std::min(zw - 1, (int)std::ceil(x1)); ++px) {
          const double qx = px + 0.5, qy = py + 0.5;
          const double l0 = ((sp[b].y - sp[c].y) * (qx - sp[c].x) + (sp[c].x - sp[b].x) * (qy - sp[c].y)) / den;
          const double l1 = ((sp[c].y - sp[a].y) * (qx - sp[c].x) + (sp[a].x - sp[c].x) * (qy - sp[c].y)) / den;
          const double l2 = 1 - l0 - l1;
          if (l0 < -1e-6 || l1 < -1e-6 || l2 < -1e-6) continue;
          const float d = (float)(l0 * sd[a] + l1 * sd[b] + l2 * sd[c]);
          float& z = zb[(size_t)py * zw + px];
          if (d < z) z = d;
        }
    }
    double from[3];
    for (int i = 0; i < 3; ++i) from[i] = -(v.cam.R[i] * v.cam.t[0] + v.cam.R[3 + i] * v.cam.t[1] + v.cam.R[6 + i] * v.cam.t[2]);
    for (size_t i = 0; i < nv; ++i) {
      if (!ok[i]) continue;
      const int px = (int)sp[i].x, py = (int)sp[i].y;
      if (px < 0 || py < 0 || px >= zw || py >= zh) continue;
      // seen: no nearer surface within a cell and a half (a 3x3 neighbourhood, for the rasterized edges)
      float zmin = 1e30f;
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          const int qx = px + dx, qy = py + dy;
          if (qx >= 0 && qy >= 0 && qx < zw && qy < zh) zmin = std::min(zmin, zb[(size_t)qy * zw + qx]);
        }
      if (sd[i] > zmin + 1.5 * s) continue;
      double d[3] = {from[0] - P[i * 3], from[1] - P[i * 3 + 1], from[2] - P[i * 3 + 2]};
      const double l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      const double facing = (d[0] * Nn[i * 3] + d[1] * Nn[i * 3 + 1] + d[2] * Nn[i * 3 + 2]) / l;
      if (facing <= 0.05) continue;
      const double wgt = facing * facing * facing;
      // the photo's colour there, bilinear, in the picture's own pixels
      const double ix = sp[i].x / k - 0.5, iy = sp[i].y / k - 0.5;
      const int x0 = std::clamp((int)std::floor(ix), 0, (int)v.iw - 1), y0 = std::clamp((int)std::floor(iy), 0, (int)v.ih - 1);
      const int x1 = std::min<int>(x0 + 1, v.iw - 1), y1 = std::min<int>(y0 + 1, v.ih - 1);
      const double tx = std::clamp(ix - x0, 0.0, 1.0), ty = std::clamp(iy - y0, 0.0, 1.0);
      for (int ch = 0; ch < 3; ++ch) {
        const int sh = 16 - ch * 8;
        auto g = [&](int xx, int yy) { return (double)(((*v.img)[(size_t)yy * v.iw + xx] >> sh) & 255); };
        const double val = (g(x0, y0) * (1 - tx) + g(x1, y0) * tx) * (1 - ty) + (g(x0, y1) * (1 - tx) + g(x1, y1) * tx) * ty;
        C[i * 3 + ch] += wgt * val / 255.0;
      }
      Wt[i] += wgt;
    }
  }
  size_t coloured = 0;
  for (double w : Wt) coloured += w > 0;
  // what no photo sees (the underside, on the mat) takes its neighbours' colour, spreading in from the seen
  {
    for (size_t i = 0; i < nv; ++i) if (Wt[i] > 0) for (int k = 0; k < 3; ++k) C[i * 3 + k] /= Wt[i];
    std::vector<uint8_t> have(nv);
    for (size_t i = 0; i < nv; ++i) have[i] = Wt[i] > 0;
    std::vector<std::vector<uint32_t>> nb(nv);
    for (size_t t = 0; t + 2 < I.size(); t += 3)
      for (int a = 0; a < 3; ++a) nb[I[t + a]].push_back(I[t + (a + 1) % 3]);
    bool grew = coloured > 0;
    while (grew) {
      grew = false;
      std::vector<uint32_t> now;
      for (size_t i = 0; i < nv; ++i) {
        if (have[i]) continue;
        double acc[3] = {0, 0, 0};
        int cnt = 0;
        for (uint32_t j : nb[i]) if (have[j]) { for (int k = 0; k < 3; ++k) acc[k] += C[(size_t)j * 3 + k]; ++cnt; }
        if (cnt == 0) continue;
        for (int k = 0; k < 3; ++k) C[i * 3 + k] = acc[k] / cnt;
        now.push_back((uint32_t)i);
      }
      for (uint32_t i : now) have[i] = 1;
      grew = !now.empty();
    }
    for (size_t i = 0; i < nv; ++i) if (!have[i]) for (int k = 0; k < 3; ++k) C[i * 3 + k] = 0.5;
  }
  std::vector<uint8_t> Pb, Nb, Cb, Ib;
  double vol = 0;
  for (size_t i = 0; i < nv; ++i) {
    for (int k = 0; k < 3; ++k) { put_f32(Pb, (float)P[i * 3 + k]); put_f32(Nb, (float)Nn[i * 3 + k]); }
    for (int k = 0; k < 3; ++k) put_f32(Cb, (float)C[i * 3 + k]);
  }
  for (size_t t = 0; t + 2 < I.size(); t += 3) {
    const double* a = &P[(size_t)I[t] * 3]; const double* b = &P[(size_t)I[t + 1] * 3]; const double* c = &P[(size_t)I[t + 2] * 3];
    vol += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0;
    for (int k = 0; k < 3; ++k) put_u32(Ib, I[t + k]);
  }
  double mn[3] = {1e30, 1e30, 1e30}, mx[3] = {-1e30, -1e30, -1e30};
  for (size_t i = 0; i < nv; ++i) for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], P[i * 3 + k]); mx[k] = std::max(mx[k], P[i * 3 + k]); }
  auto res_m = std::make_shared<MapObj>();
  res_m->put("positions", Value::bytes(std::move(Pb)));
  res_m->put("normals", Value::bytes(std::move(Nb)));
  res_m->put("colors", Value::bytes(std::move(Cb)));
  res_m->put("indices", Value::bytes(std::move(Ib)));
  res_m->put("vertex_count", Value::integer((int64_t)nv));
  res_m->put("tris", Value::integer((int64_t)(I.size() / 3)));
  res_m->put("volume_mm3", Value::real(vol));
  res_m->put("voxels", Value::integer((int64_t)voxels));
  res_m->put("cell_mm", Value::real(s));
  res_m->put("coloured", Value::integer((int64_t)coloured));
  res_m->put("min", Value::list({Value::real(mn[0]), Value::real(mn[1]), Value::real(mn[2])}));
  res_m->put("max", Value::list({Value::real(mx[0]), Value::real(mx[1]), Value::real(mx[2])}));
  return Value::object(Type::Map, res_m);
}

// image_difference(a, b[, opts]) -> {changed, compared, gain, mask, w, h}: where two pictures of the same view (the
// same size) show different things, under whatever light each was taken in. In order:
//   - the light's colour: each channel of a scaled by the median ratio b / a of that channel (sodium orange against a
//     white work light is not a change);
//   - the light's spread: a smooth field (a quadratic in x and y, fitted to the log-brightness ratio with the outliers
//     weighed down) divided out (a hot spot, a vignette, light falling off with distance);
//   - then each pixel by its neighbourhood (9 x 9), against b's within opts.shift cells (3) each way, the best match
//     kept (so a few cells of misregistration between the views do not count): where a shows texture (dirt, grain,
//     edges), changed when no neighbourhood of b nearby shows the same pattern (normalized correlation of at least
//     opts.ncc, 0.6) - a shadow or a patch of extra light keeps the pattern, something put there replaces it; where a
//     is plain, changed when no plain neighbourhood of b nearby is within a factor of opts.ratio (1.6) of its
//     brightness and opts.chroma (0.06) of its colour.
//   Only pixels both pictures have (alpha) count; specks of one cell are dropped. changed: the share of those;
//   compared: their share of the picture; gain: the channels' scales.
Value bi_image_difference(Vm&, std::vector<Value>& args, std::string& error) {
  uint32_t W = 0, H = 0, W2 = 0, H2 = 0;
  const std::vector<uint32_t>* a = nullptr;
  const std::vector<uint32_t>* b = nullptr;
  if (!canvas_pixels(args[0], &W, &H, &a, error, "image_difference") || !canvas_pixels(args[1], &W2, &H2, &b, error, "image_difference")) return Value::nil();
  if (W != W2 || H != H2) { error = "image_difference(): the pictures are " + std::to_string(W) + "x" + std::to_string(H) + " and " + std::to_string(W2) + "x" + std::to_string(H2) + ": resample one to the other first"; return Value::nil(); }
  MapObj* o = opts_map(args, 2);
  const double ncc_min = std::clamp(opt_num(o, "ncc", 0.6), -1.0, 1.0);
  const double ratio = std::max(1.01, opt_num(o, "ratio", 1.6));
  const double chroma_max = std::clamp(opt_num(o, "chroma", 0.06), 0.001, 1.0);
  const size_t n = (size_t)W * H;
  const auto& T = lab_table();
  // linear channels, blurred 3 x 3 (the sensor's grain)
  auto lin = [&](const std::vector<uint32_t>& px) {
    std::vector<float> L(n * 3);
    for (uint32_t y = 0; y < H; ++y)
      for (uint32_t x = 0; x < W; ++x) {
        double c[3] = {0, 0, 0};
        int cnt = 0;
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx) {
            const int xx = (int)x + dx, yy = (int)y + dy;
            if (xx < 0 || yy < 0 || xx >= (int)W || yy >= (int)H) continue;
            const uint32_t p = px[(size_t)yy * W + xx];
            c[0] += T.lin[(p >> 16) & 255]; c[1] += T.lin[(p >> 8) & 255]; c[2] += T.lin[p & 255]; ++cnt;
          }
        for (int k = 0; k < 3; ++k) L[((size_t)y * W + x) * 3 + k] = (float)(c[k] / cnt);
      }
    return L;
  };
  std::vector<float> A = lin(*a), B = lin(*b);
  std::vector<uint8_t> use(n);
  size_t nuse = 0;
  for (size_t i = 0; i < n; ++i) { use[i] = ((*a)[i] >> 24) >= 128 && ((*b)[i] >> 24) >= 128; nuse += use[i]; }
  if (nuse == 0) { error = "image_difference(): the pictures have no pixel in common"; return Value::nil(); }
  const float eps = 0.004f;
  // the light's colour: a per-channel scale (the median ratio)
  double gain[3];
  for (int k = 0; k < 3; ++k) {
    std::vector<double> r;
    r.reserve(nuse);
    for (size_t i = 0; i < n; ++i) if (use[i]) r.push_back((B[i * 3 + k] + eps) / (A[i * 3 + k] + eps));
    std::nth_element(r.begin(), r.begin() + r.size() / 2, r.end());
    gain[k] = r[r.size() / 2];
    for (size_t i = 0; i < n; ++i) A[i * 3 + k] = (float)(A[i * 3 + k] * gain[k]);
  }
  // log brightness, and chromaticity
  std::vector<double> la(n), lb(n), ca(n * 2), cb(n * 2);
  for (size_t i = 0; i < n; ++i) {
    const double sa = A[i * 3] + A[i * 3 + 1] + A[i * 3 + 2] + 3 * eps, sb = B[i * 3] + B[i * 3 + 1] + B[i * 3 + 2] + 3 * eps;
    la[i] = std::log(sa); lb[i] = std::log(sb);
    ca[i * 2] = (A[i * 3] + eps) / sa; ca[i * 2 + 1] = (A[i * 3 + 1] + eps) / sa;
    cb[i * 2] = (B[i * 3] + eps) / sb; cb[i * 2 + 1] = (B[i * 3 + 1] + eps) / sb;
  }
  // the light's spread: d = lb - la ~ c0 + c1 x + c2 y + c3 x^2 + c4 xy + c5 y^2, by least squares weighted against
  // outliers (what changed) over a few rounds
  double coef[6] = {0, 0, 0, 0, 0, 0};
  {
    std::vector<double> wt(n, 1.0);
    for (int round = 0; round < 5; ++round) {
      double M[6][7] = {{0}};
      for (uint32_t y = 0; y < H; y += 2)
        for (uint32_t x = 0; x < W; x += 2) {
          const size_t i = (size_t)y * W + x;
          if (!use[i]) continue;
          const double u = (2.0 * x / W) - 1, v = (2.0 * y / H) - 1;
          const double f[6] = {1, u, v, u * u, u * v, v * v};
          const double d = lb[i] - la[i], w = wt[i];
          for (int p = 0; p < 6; ++p) { for (int q = 0; q < 6; ++q) M[p][q] += w * f[p] * f[q]; M[p][6] += w * f[p] * d; }
        }
      for (int p = 0; p < 6; ++p) M[p][p] += 1e-6;
      bool ok = true;
      for (int c = 0; c < 6 && ok; ++c) {
        int piv = c;
        for (int r = c + 1; r < 6; ++r) if (std::fabs(M[r][c]) > std::fabs(M[piv][c])) piv = r;
        if (std::fabs(M[piv][c]) < 1e-12) { ok = false; break; }
        for (int k = 0; k < 7; ++k) std::swap(M[c][k], M[piv][k]);
        for (int r = 0; r < 6; ++r) { if (r == c) continue; const double f = M[r][c] / M[c][c]; for (int k = c; k < 7; ++k) M[r][k] -= f * M[c][k]; }
      }
      if (!ok) break;
      for (int p = 0; p < 6; ++p) coef[p] = M[p][6] / M[p][p];
      // Tukey's weights at four times the residuals' median spread
      std::vector<double> res;
      for (uint32_t y = 0; y < H; y += 2)
        for (uint32_t x = 0; x < W; x += 2) {
          const size_t i = (size_t)y * W + x;
          if (!use[i]) continue;
          const double u = (2.0 * x / W) - 1, v = (2.0 * y / H) - 1;
          res.push_back(std::fabs(lb[i] - la[i] - (coef[0] + coef[1] * u + coef[2] * v + coef[3] * u * u + coef[4] * u * v + coef[5] * v * v)));
        }
      std::nth_element(res.begin(), res.begin() + res.size() / 2, res.end());
      const double c4 = std::max(0.05, 4.0 * 1.4826 * res[res.size() / 2]);
      for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
          const size_t i = (size_t)y * W + x;
          const double u = (2.0 * x / W) - 1, v = (2.0 * y / H) - 1;
          const double e = std::fabs(lb[i] - la[i] - (coef[0] + coef[1] * u + coef[2] * v + coef[3] * u * u + coef[4] * u * v + coef[5] * v * v)) / c4;
          wt[i] = e < 1 ? (1 - e * e) * (1 - e * e) : 0;
        }
    }
    for (uint32_t y = 0; y < H; ++y)
      for (uint32_t x = 0; x < W; ++x) {
        const size_t i = (size_t)y * W + x;
        const double u = (2.0 * x / W) - 1, v = (2.0 * y / H) - 1;
        la[i] += coef[0] + coef[1] * u + coef[2] * v + coef[3] * u * u + coef[4] * u * v + coef[5] * v * v;
      }
  }
  // the neighbourhoods (9 x 9), by summed-area tables; b's taken at every shift up to opts.shift cells (3) each way and
  // the best match kept - where the two views' registration is a few cells out (a camera found from a small marker),
  // what is the same still finds itself; what was put there finds nothing like it near
  const int R = 4, SH = (int)std::clamp(opt_num(o, "shift", 3), 0.0, 8.0);
  auto table = [&](auto f) {
    std::vector<double> S((size_t)(W + 1) * (H + 1), 0);
    for (uint32_t y = 0; y < H; ++y)
      for (uint32_t x = 0; x < W; ++x)
        S[(size_t)(y + 1) * (W + 1) + x + 1] = f((int)x, (int)y) + S[(size_t)y * (W + 1) + x + 1] + S[(size_t)(y + 1) * (W + 1) + x] - S[(size_t)y * (W + 1) + x];
    return S;
  };
  auto ok_at = [&](int x, int y) { return x >= 0 && y >= 0 && x < (int)W && y < (int)H && use[(size_t)y * W + x]; };
  const auto Sn = table([&](int x, int y) { return (double)ok_at(x, y); });
  const auto Sa = table([&](int x, int y) { return ok_at(x, y) ? la[(size_t)y * W + x] : 0.0; });
  const auto Saa = table([&](int x, int y) { return ok_at(x, y) ? la[(size_t)y * W + x] * la[(size_t)y * W + x] : 0.0; });
  const auto Sb = table([&](int x, int y) { return ok_at(x, y) ? lb[(size_t)y * W + x] : 0.0; });
  const auto Sbb = table([&](int x, int y) { return ok_at(x, y) ? lb[(size_t)y * W + x] * lb[(size_t)y * W + x] : 0.0; });
  auto box = [&](const std::vector<double>& S, int x0, int y0, int x1, int y1) {
    x0 = std::clamp(x0, 0, (int)W); x1 = std::clamp(x1, 0, (int)W); y0 = std::clamp(y0, 0, (int)H); y1 = std::clamp(y1, 0, (int)H);
    if (x1 <= x0 || y1 <= y0) return 0.0;
    return S[(size_t)y1 * (W + 1) + x1] - S[(size_t)y0 * (W + 1) + x1] - S[(size_t)y1 * (W + 1) + x0] + S[(size_t)y0 * (W + 1) + x0];
  };
  const double tex_min = 0.06, log_ratio = std::log(ratio);
  // per pixel: the best correlation found (textured), and the smallest brightness and colour difference (plain)
  std::vector<double> best_ncc(n, -2), best_dl(n, 1e9), best_dc(n, 1e9);
  std::vector<uint8_t> tex_a(n, 0), tex_b_any(n, 0), plain_b_any(n, 0);
  for (int dy = -SH; dy <= SH; ++dy)
    for (int dx = -SH; dx <= SH; ++dx) {
      // a(p) b(p + d)
      const auto Sab = table([&](int x, int y) { return ok_at(x, y) && ok_at(x + dx, y + dy) ? la[(size_t)y * W + x] * lb[(size_t)(y + dy) * W + x + dx] : 0.0; });
      for (int y = 0; y < (int)H; ++y)
        for (int x = 0; x < (int)W; ++x) {
          const size_t i = (size_t)y * W + x;
          if (!use[i] || !ok_at(x + dx, y + dy)) continue;
          const int x0 = x - R, y0 = y - R, x1 = x + R + 1, y1 = y + R + 1;
          const double ca_n = box(Sn, x0, y0, x1, y1), cb_n = box(Sn, x0 + dx, y0 + dy, x1 + dx, y1 + dy);
          if (ca_n < 9 || cb_n < 9) continue;
          const double ma = box(Sa, x0, y0, x1, y1) / ca_n, mb = box(Sb, x0 + dx, y0 + dy, x1 + dx, y1 + dy) / cb_n;
          const double va = std::max(0.0, box(Saa, x0, y0, x1, y1) / ca_n - ma * ma);
          const double vb = std::max(0.0, box(Sbb, x0 + dx, y0 + dy, x1 + dx, y1 + dy) / cb_n - mb * mb);
          const bool ta = std::sqrt(va) > tex_min, tb = std::sqrt(vb) > tex_min;
          tex_a[i] = ta;
          if (tb) tex_b_any[i] = 1; else plain_b_any[i] = 1;
          if (ta && tb) {
            const double cov = box(Sab, x0, y0, x1, y1) / ca_n - ma * mb;
            best_ncc[i] = std::max(best_ncc[i], cov / std::sqrt(va * vb));
          }
          if (!ta && !tb) {
            const size_t j = (size_t)(y + dy) * W + x + dx;
            best_dl[i] = std::min(best_dl[i], std::fabs(mb - ma));
            best_dc[i] = std::min(best_dc[i], std::hypot(ca[i * 2] - cb[j * 2], ca[i * 2 + 1] - cb[j * 2 + 1]));
          }
        }
    }
  std::vector<uint8_t> m(n, 0);
  for (size_t i = 0; i < n; ++i) {
    if (!use[i]) continue;
    if (tex_a[i]) m[i] = !(tex_b_any[i] && best_ncc[i] >= ncc_min);       // a's pattern found nowhere near in b
    else m[i] = !(plain_b_any[i] && best_dl[i] <= log_ratio && best_dc[i] <= chroma_max);
  }
  morph(m, (int)W, (int)H, 1, false);
  morph(m, (int)W, (int)H, 1, true);
  size_t cnt = 0;
  std::vector<uint8_t> out(n);
  for (size_t i = 0; i < n; ++i) { cnt += m[i] && use[i]; out[i] = m[i] && use[i] ? 255 : 0; }
  auto res = std::make_shared<MapObj>();
  res->put("changed", Value::real((double)cnt / nuse));
  res->put("compared", Value::real((double)nuse / n));
  res->put("gain", Value::list({Value::real(gain[0]), Value::real(gain[1]), Value::real(gain[2])}));
  res->put("mask", Value::bytes(std::move(out)));
  res->put("w", Value::integer(W));
  res->put("h", Value::integer(H));
  return Value::object(Type::Map, res);
}

// mesh_lowest(positions, poses) -> [z ...]: for each pose [qx, qy, qz, qw, sz] (a rotation, after stretching z by
// sz), the lowest height any of the points reaches - positions: bytes of float32 xyz, or a list of such bytes
Value bi_mesh_lowest(Vm&, std::vector<Value>& args, std::string& error) {
  std::vector<const std::vector<uint8_t>*> sets;
  if (args[0].type == Type::Bytes) sets.push_back(&args[0].bytes_data());
  else if (args[0].type == Type::List) { for (const Value& v : args[0].list_items()) if (v.type == Type::Bytes) sets.push_back(&v.bytes_data()); }
  if (sets.empty()) { error = "mesh_lowest() wants positions: bytes of float32 xyz, or a list of them"; return Value::nil(); }
  if (args[1].type != Type::List) { error = "mesh_lowest() wants a list of poses [qx, qy, qz, qw, sz]"; return Value::nil(); }
  std::vector<Value> out;
  for (const Value& pv : args[1].list_items()) {
    if (pv.type != Type::List || pv.list_items().size() < 5) { error = "mesh_lowest(): each pose is [qx, qy, qz, qw, sz]"; return Value::nil(); }
    const auto& q = pv.list_items();
    const double x = q[0].as_double(), y = q[1].as_double(), z = q[2].as_double(), w = q[3].as_double(), sz = q[4].as_double();
    const double r0 = 2 * (x * z - w * y), r1 = 2 * (y * z + w * x), r2 = 1 - 2 * (x * x + y * y);
    double low = 1e300;
    for (const auto* b : sets)
      for (size_t i = 0; i + 12 <= b->size(); i += 12) {
        float px, py, pz;
        std::memcpy(&px, &(*b)[i], 4); std::memcpy(&py, &(*b)[i + 4], 4); std::memcpy(&pz, &(*b)[i + 8], 4);
        low = std::min(low, r0 * px + r1 * py + r2 * pz * sz);
      }
    out.push_back(Value::real(low));
  }
  return Value::list(std::move(out));
}

// ---- tires ----------------------------------------------------------------------------------------------------

Value bi_tire_size(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::Str) { error = std::string("tire_size() wants a size as a string, got a ") + type_name(args[0].type); return Value::nil(); }
  const tire::Size z = tire::parse_size(args[0].str());
  if (!z.ok) return Value::nil();
  auto m = std::make_shared<MapObj>();
  m->put("width_mm", Value::real(z.width_mm));
  m->put("diameter_mm", Value::real(z.diameter_mm));
  return Value::object(Type::Map, m);
}

Value bi_tire_fit(Vm&, std::vector<Value>& args, std::string& error) {
  for (int i = 0; i < 3; ++i) if (!want_number(args[(size_t)i], error, "tire_fit")) return Value::nil();
  double tw = 0, td = 0;
  if (args[3].type == Type::Str) {
    const tire::Size z = tire::parse_size(args[3].str());
    if (!z.ok) { error = "tire_fit(): \"" + args[3].str() + "\" is not a tire size"; return Value::nil(); }
    tw = z.width_mm; td = z.diameter_mm;
  } else {
    if (args.size() < 5 || !want_number(args[3], error, "tire_fit") || !want_number(args[4], error, "tire_fit")) {
      if (error.empty()) error = "tire_fit() wants a size, or a width and a diameter (mm)";
      return Value::nil();
    }
    tw = args[3].as_double(); td = args[4].as_double();
  }
  const tire::Fit f = tire::fit(args[0].as_double(), args[1].as_double(), args[2].as_double(), tw, td);
  auto m = std::make_shared<MapObj>();
  m->put("count", Value::integer(f.count));
  m->put("mode", Value::string(f.mode));
  return Value::object(Type::Map, m);
}

}  // namespace

void install_carve_builtins(Vm& vm) {
  vm.define_native("image_cutout", 1, 2, bi_image_cutout);
  vm.define_native("image_quantize", 2, 3, bi_image_quantize);
  vm.define_native("image_resample", 3, 4, bi_image_resample);
  vm.define_native("mesh_inflate", 3, 4, bi_mesh_inflate);
  vm.define_native("image_find_marker", 1, 2, bi_image_find_marker);
  vm.define_native("homography", 2, 2, bi_homography);
  vm.define_native("homography_apply", 2, 2, bi_homography_apply);
  vm.define_native("image_rectify", 4, 4, bi_image_rectify);
  vm.define_native("markers_find", 1, 2, bi_markers_find);
  vm.define_native("marker_image", 2, 2, bi_marker_image);
  vm.define_native("camera_pose", 4, 5, bi_camera_pose);
  vm.define_native("camera_project", 2, 2, bi_camera_project);
  vm.define_native("camera_ray", 2, 2, bi_camera_ray);
  vm.define_native("camera_box_mask", 4, 4, bi_camera_box_mask);
  vm.define_native("visual_hull", 2, 3, bi_visual_hull);
  vm.define_native("image_difference", 2, 3, bi_image_difference);
  vm.define_native("camera_pose_rect", 5, 6, bi_camera_pose_rect);
  vm.define_native("box_face_views", 6, 6, bi_box_face_views);
  vm.define_native("mesh_lowest", 2, 2, bi_mesh_lowest);
  vm.define_native("tire_size", 1, 1, bi_tire_size);
  vm.define_native("tire_fit", 4, 5, bi_tire_fit);
}

}  // namespace aios::userland::grain
