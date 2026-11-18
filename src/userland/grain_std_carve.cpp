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
#include <deque>
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

  // whether anything stands out at all: how many cells are far (3 spreads) from every colour of the border
  const std::vector<Cluster> border_colours = kmeans(border, 5);
  {
    size_t far = 0;
    for (size_t i = 0; i < n; ++i) if (!sure_bg[i] && cluster_distance(L[i], border_colours, 2.5f) >= 3.0f) ++far;
    if (far * 1000 < n) { error = "image_cutout(): nothing stands out from the background (a plainer background, or the subject filling more of the picture, helps)"; return Value::nil(); }
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
      const uint8_t v = sure_bg[i] ? 0 : (g.source_side((int)i) ? 1 : 0);
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

Value bi_image_find_marker(Vm&, std::vector<Value>& args, std::string& error) {
  uint32_t W = 0, H = 0;
  const std::vector<uint32_t>* px = nullptr;
  if (!canvas_pixels(args[0], &W, &H, &px, error, "image_find_marker")) return Value::nil();
  MapObj* o = opts_map(args, 1);
  const int max_side = (int)std::clamp(opt_num(o, "max_side", 1200), 128.0, 4096.0);
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
  struct Cand { P2 c[4]; double score; double side; };
  std::vector<Cand> cands;
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
    if (hole_g / hole < piece_g / sizes[id] + 40.0) continue;
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
    // a filled square with a centred hole of about a quarter: not a tire, not a shadow, not text
    if (fill < 0.85 || fill > 1.15 || hole_share < 0.13 || hole_share > 0.42 || off > 0.12) continue;
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
    Cand cd;
    for (int i = 0; i < 4; ++i) cd.c[i] = rq[(tl + i) % 4];
    cd.side = std::sqrt(quad_area(cd.c));
    cd.score = (1.0 - std::fabs(fill - 1.0)) * (1.0 - std::fabs(hole_share - 0.25)) * (1.0 - off);
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
  vm.define_native("tire_size", 1, 1, bi_tire_size);
  vm.define_native("tire_fit", 4, 5, bi_tire_fit);
}

}  // namespace aios::userland::grain
