// r3d_*: the pixel half of a 3D renderer, for a program whose other half - the scene, the camera, the transforms,
// culling and lighting - is somewhere else (for apps/pickrush, in ASQL). The caller hands over triangles already in
// camera space (x right, y up, z forward, in world units) with a flat colour each; this clips them against the near
// plane, projects them and fills them with a depth buffer. It is deliberately nothing more: a vertex stage in SQL
// and a fill stage here are the same split a GPU makes, and the fill is the part no query should be doing per pixel.
//
//   r3d_target(w, h)               -> a target: a colour buffer and a depth buffer
//   r3d_clear(t, color)            -> t, colour set to `color` (0xRRGGBB), depth to "nothing drawn"
//   r3d_draw(t, rows, fov_deg)     -> how many triangles were filled
//       rows: a list of maps, as sql() returns them, each with x0 y0 z0 x1 y1 z1 x2 y2 z2 and col (0xRRGGBB);
//       an optional em (0..7) marks which edges (bit 0: v0-v1, bit 1: v1-v2, bit 2: v2-v0) are drawn darker, so a
//       box made of triangles shows its edges and not its diagonals. fov_deg is the horizontal field of view.
//   r3d_pixels(t)                  -> the colour buffer as bytes, 4 a pixel (R, G, B, A), for canvas_set_pixels
//   r3d_stats(t)                   -> {tris, filled, clipped, pixels}: the last draw's counts
#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <optional>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "aios/userland/grain.hpp"
#include "aios/userland/sql_database.hpp"   // SqlThreadPool, for the textured fill's bands
#include "grain_builtins_internal.hpp"

namespace aios::userland::grain {

namespace {

// A texture as the textured fill reads it: its pictures from full size down to 1x1, each texel 0xAARRGGBB.
struct R3dTex {
  struct Level {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
  };
  std::vector<Level> mips;
  Level sky_smooth;     // r3d_sky's: level 0 smoothed along its rows, made the first time it is a sky
  bool alpha = false;   // some texel is transparent
  int scale = 1;        // texels per texel of the picture as given (r3d_options' upscale when it was uploaded)
};

// One view of a sprite frame: its picture (a texture id), mirrored or not, and where its origin is in it.
struct R3dView { int64_t tex = 0; bool flip = false; double left = 0, top = 0; };

// A sprite frame: one view (seen the same from everywhere) or eight (rotations 1..8: the viewer at the thing's
// facing + 45 degrees each, counter-clockwise).
struct R3dSet {
  std::vector<R3dView> views;
};

// A point light: where (in the world as r3d_lights was given it; turned to the camera for each draw), its colour
// (each channel 0..1 and past: how much of the surface's own colour it adds at its heart) and how far it reaches.
struct R3dLight { double x, y, z, r, g, b, radius; };

struct R3dTarget {
  int w = 0, h = 0;
  std::vector<R3dLight> lights_world;   // r3d_lights
  std::vector<R3dLight> lights_cam;     // the same in camera space, for the draw under way
  std::vector<uint32_t> color;  // 0x00RRGGBB
  std::vector<float> depth;     // 1/z of what is drawn there; 0 is nothing (an infinitely far background)
  int64_t tris = 0, filled = 0, clipped = 0, pixels = 0;
  // the textured fill's
  std::vector<std::shared_ptr<R3dTex>> tex;   // by id
  bool bilinear = true, mipmaps = true;
  // Doom's light: a surface of light level L at distance z is colormap c = 60 - L/4 - light_fall / z (Doom's 1280:
  // its lightnum's start map less the scale's), 0 the brightest of 32 and 31 the darkest, so brightness is
  // 1 - c/32 - here not cut into 32 steps. light_gain scales how dark that gets, light_lift brightens all.
  double light_gain = 1.0, light_lift = 0.0, light_fall = 1280.0;
  double y_scale = 1.0;   // vertical pixels per horizontal one in projection (Doom's 320x200 on 4:3 is 1.2)
  std::unordered_map<int64_t, R3dSet> sets;   // r3d_sprite_set's, by id
  int upscale = 1;       // what r3d_texture enlarges a picture by (1, 2 or 4), with the pixel-art scaler
  double focal = 1;      // the last r3d_draw_tex's focal length in pixels, for the fill's eye directions
  double pitch = 0;      // the last r3d_draw_tex camera's pitch (radians): the sky follows it
  double time = 0;       // seconds, for what moves on its own (a spectre's shimmer, liquids): r3d_options' time
  std::unordered_map<int64_t, std::shared_ptr<struct R3dMesh>> meshes;   // r3d_mesh's, by id
  std::unordered_map<int64_t, std::shared_ptr<struct R3dModel>> models;  // r3d_model's, by id
};

std::shared_ptr<R3dTarget> get_target(const Value& v, std::string& error, const char* who) {
  if (v.type == Type::Extern) {
    auto ext = static_cast<ExternObj*>(v.obj.get());
    if (ext->kind_name == "r3d_target" && ext->ptr) return std::static_pointer_cast<R3dTarget>(ext->ptr);
  }
  error = std::string(who) + "() wants an r3d target (r3d_target) as its first argument, got a " + type_name(v.type);
  return nullptr;
}

struct V3 { double x, y, z; };

// One field of a row as a number; false when it is missing or not a number.
bool field(MapObj* m, const std::string& k, double& out) {
  Value* v = m->find(k);
  if (!v || !(v->is_number())) return false;
  out = v->as_double();
  return true;
}

constexpr double kNear = 0.05;

// Fill one triangle already in screen space: sx, sy in pixels, iz = 1/z. `em` marks the edges to darken.
void fill(R3dTarget& t, const double* sx, const double* sy, const double* iz, uint32_t col, int em) {
  double area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
  if (std::fabs(area) < 1e-9) return;
  // Either winding: the caller culls back faces; a clipped triangle keeps its winding but a caller's may differ.
  const double sgn = area > 0 ? 1.0 : -1.0;
  area = std::fabs(area);
  int x0 = (int)std::floor(std::min({sx[0], sx[1], sx[2]}));
  int x1 = (int)std::ceil(std::max({sx[0], sx[1], sx[2]}));
  int y0 = (int)std::floor(std::min({sy[0], sy[1], sy[2]}));
  int y1 = (int)std::ceil(std::max({sy[0], sy[1], sy[2]}));
  x0 = std::max(x0, 0); y0 = std::max(y0, 0);
  x1 = std::min(x1, t.w - 1); y1 = std::min(y1, t.h - 1);
  if (x0 > x1 || y0 > y1) return;
  ++t.filled;

  // Edge i runs from vertex i to vertex i+1; its function is positive inside (after the winding sign).
  double ea[3], eb[3], ec[3], elen[3];
  for (int i = 0; i < 3; ++i) {
    const int j = (i + 1) % 3;
    ea[i] = sgn * (sy[i] - sy[j]);
    eb[i] = sgn * (sx[j] - sx[i]);
    ec[i] = sgn * (sx[i] * sy[j] - sx[j] * sy[i]);
    elen[i] = std::sqrt(ea[i] * ea[i] + eb[i] * eb[i]);
  }
  // Edge i is opposite vertex (i+2)%3: the weight of that vertex is edge i's function over the area.
  const uint32_t r = (col >> 16) & 0xFF, g = (col >> 8) & 0xFF, b = col & 0xFF;
  const uint32_t dark = ((r * 5 / 8) << 16) | ((g * 5 / 8) << 8) | (b * 5 / 8);
  const uint32_t base = (r << 16) | (g << 8) | b;
  // 1/z is linear in screen space: its value at a pixel is (e1 iz0 + e2 iz1 + e0 iz2) / area (vertex 2 weighted
  // by edge 0, vertex 0 by edge 1, vertex 1 by edge 2), so along a row it steps by a constant.
  const double zx = (ea[1] * iz[0] + ea[2] * iz[1] + ea[0] * iz[2]) / area;
  const double zy = (eb[1] * iz[0] + eb[2] * iz[1] + eb[0] * iz[2]) / area;
  const double zc = (ec[1] * iz[0] + ec[2] * iz[1] + ec[0] * iz[2]) / area;
  // a marked edge's function over its length is the distance to it in pixels
  double inv_len[3];
  for (int i = 0; i < 3; ++i) inv_len[i] = ((em >> i) & 1) && elen[i] > 0 ? 1.0 / elen[i] : 0.0;
  for (int y = y0; y <= y1; ++y) {
    const double py = y + 0.5;
    // The row's span: each edge function is a x + c along it, so its non-negative part is a half-line of x.
    double lo = x0 + 0.5, hi = x1 + 0.5;
    double c[3];
    bool empty = false;
    for (int i = 0; i < 3 && !empty; ++i) {
      c[i] = eb[i] * py + ec[i];
      if (ea[i] > 0) lo = std::max(lo, -c[i] / ea[i]);
      else if (ea[i] < 0) hi = std::min(hi, -c[i] / ea[i]);
      else if (c[i] < 0) empty = true;
    }
    if (empty || lo > hi) continue;
    // pixel centres in [lo, hi], widened by one on each side and then tested exactly, so rounding at the span's
    // ends never drops or adds a pixel the edge functions would not
    int xa = std::max(x0, (int)std::floor(lo - 0.5) - 1);
    int xb = std::min(x1, (int)std::ceil(hi - 0.5) + 1);
    float* drow = &t.depth[(size_t)y * t.w];
    uint32_t* crow = &t.color[(size_t)y * t.w];
    double px = xa + 0.5;
    double e0 = ea[0] * px + c[0], e1 = ea[1] * px + c[1], e2 = ea[2] * px + c[2];
    double z = zx * px + zy * py + zc;
    for (int x = xa; x <= xb; ++x, e0 += ea[0], e1 += ea[1], e2 += ea[2], z += zx) {
      if (e0 < 0 || e1 < 0 || e2 < 0) continue;
      const float zf = (float)z;
      if (zf <= drow[x]) continue;
      drow[x] = zf;
      const bool edge = em && ((e0 * inv_len[0] < 1.0 && inv_len[0] > 0) || (e1 * inv_len[1] < 1.0 && inv_len[1] > 0) ||
                               (e2 * inv_len[2] < 1.0 && inv_len[2] > 0));
      crow[x] = edge ? dark : base;
      ++t.pixels;
    }
  }
}

// Project a camera-space polygon (3 or 4 points, all in front of the near plane) and fill it as triangles.
void project_fill(R3dTarget& t, const V3* p, const int* pem, int n, double focal, uint32_t col) {
  double sx[4], sy[4], iz[4];
  const double cx = t.w * 0.5, cy = t.h * 0.5;
  for (int i = 0; i < n; ++i) {
    iz[i] = 1.0 / p[i].z;
    sx[i] = cx + p[i].x * focal * iz[i];
    sy[i] = cy - p[i].y * focal * iz[i];
  }
  // pem[i]: whether polygon edge i (point i to i+1) is a marked edge
  {
    double ax[3] = {sx[0], sx[1], sx[2]}, ay[3] = {sy[0], sy[1], sy[2]}, az[3] = {iz[0], iz[1], iz[2]};
    int em = (pem[0] ? 1 : 0) | (pem[1] ? 2 : 0) | (n == 3 && pem[2] ? 4 : 0);
    fill(t, ax, ay, az, col, em);
  }
  if (n == 4) {
    double ax[3] = {sx[0], sx[2], sx[3]}, ay[3] = {sy[0], sy[2], sy[3]}, az[3] = {iz[0], iz[2], iz[3]};
    int em = (pem[2] ? 2 : 0) | (pem[3] ? 4 : 0);
    fill(t, ax, ay, az, col, em);
  }
}

// Clip a triangle against z = kNear (keeping z >= kNear) and fill what is left: nothing, a triangle or a quad.
void clip_fill(R3dTarget& t, const V3* v, int em, double focal, uint32_t col) {
  bool in[3] = {v[0].z >= kNear, v[1].z >= kNear, v[2].z >= kNear};
  int nin = (int)in[0] + (int)in[1] + (int)in[2];
  if (nin == 0) { ++t.clipped; return; }
  const int tri_em[3] = {em & 1, em & 2, em & 4};
  if (nin == 3) { project_fill(t, v, tri_em, 3, focal, col); return; }
  ++t.clipped;
  V3 out[4];
  int oem[4];
  int n = 0;
  for (int i = 0; i < 3; ++i) {
    const int j = (i + 1) % 3;
    const V3& a = v[i];
    const V3& b = v[j];
    if (in[i]) {
      out[n] = a;
      oem[n] = tri_em[i];  // the polygon edge from here follows edge i (or the part of it that is kept)
      ++n;
    }
    if (in[i] != in[j]) {
      const double s = (kNear - a.z) / (b.z - a.z);
      out[n] = {a.x + (b.x - a.x) * s, a.y + (b.y - a.y) * s, kNear};
      // leaving: the next polygon edge runs along the near plane (not a real edge); entering: it is edge i's rest
      oem[n] = in[i] ? 0 : tri_em[i];
      ++n;
    }
  }
  project_fill(t, out, oem, n, focal, col);
}

Value bi_r3d_target(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "r3d_target") || !want_number(args[1], error, "r3d_target")) return Value::nil();
  const int64_t w = args[0].as_int(), h = args[1].as_int();
  if (w < 1 || h < 1 || w > 8192 || h > 8192) { error = "r3d_target(): size out of range (1..8192)"; return Value::nil(); }
  auto t = std::make_shared<R3dTarget>();
  t->w = (int)w;
  t->h = (int)h;
  t->color.assign((size_t)w * h, 0);
  t->depth.assign((size_t)w * h, 0.0f);
  return Value::object(Type::Extern, std::make_shared<ExternObj>("r3d_target", t));
}

Value bi_r3d_clear(Vm&, std::vector<Value>& args, std::string& error) {
  auto t = get_target(args[0], error, "r3d_clear");
  if (!t) return Value::nil();
  uint32_t c = 0;
  if (args.size() > 1) {
    if (!want_number(args[1], error, "r3d_clear")) return Value::nil();
    c = (uint32_t)args[1].as_int() & 0xFFFFFF;
  }
  std::fill(t->color.begin(), t->color.end(), c);
  std::fill(t->depth.begin(), t->depth.end(), 0.0f);
  return args[0];
}

Value bi_r3d_draw(Vm&, std::vector<Value>& args, std::string& error) {
  auto t = get_target(args[0], error, "r3d_draw");
  if (!t) return Value::nil();
  if (!want_list(args[1], error, "r3d_draw")) return Value::nil();
  double fov = 70.0;
  if (args.size() > 2) {
    if (!want_number(args[2], error, "r3d_draw")) return Value::nil();
    fov = args[2].as_double();
  }
  if (!(fov > 1.0 && fov < 179.0)) { error = "r3d_draw(): fov_deg must be between 1 and 179"; return Value::nil(); }
  const double focal = (t->w * 0.5) / std::tan(fov * 3.14159265358979323846 / 360.0);
  t->tris = t->filled = t->clipped = t->pixels = 0;

  static const std::string kx[3] = {"x0", "x1", "x2"}, ky[3] = {"y0", "y1", "y2"}, kz[3] = {"z0", "z1", "z2"};
  static const std::string kcol = "col", kem = "em";
  const auto& rows = args[1].list_items();
  size_t at = 0;
  for (const Value& row : rows) {
    if (row.type != Type::Map) {
      error = "r3d_draw(): row " + std::to_string(at) + " is a " + type_name(row.type) + ", not a map";
      return Value::nil();
    }
    auto* m = static_cast<MapObj*>(row.obj.get());
    V3 v[3];
    for (int i = 0; i < 3; ++i) {
      if (!field(m, kx[i], v[i].x) || !field(m, ky[i], v[i].y) || !field(m, kz[i], v[i].z)) {
        error = "r3d_draw(): row " + std::to_string(at) + " needs numbers x0 y0 z0 x1 y1 z1 x2 y2 z2";
        return Value::nil();
      }
    }
    double colv = 0, emv = 0;
    if (!field(m, kcol, colv)) {
      error = "r3d_draw(): row " + std::to_string(at) + " needs a number col (0xRRGGBB)";
      return Value::nil();
    }
    field(m, kem, emv);
    ++t->tris;
    clip_fill(*t, v, (int)emv & 7, focal, (uint32_t)(int64_t)colv & 0xFFFFFF);
    ++at;
  }
  return Value::integer(t->filled);
}

Value bi_r3d_pixels(Vm&, std::vector<Value>& args, std::string& error) {
  auto t = get_target(args[0], error, "r3d_pixels");
  if (!t) return Value::nil();
  std::vector<uint8_t> out(t->color.size() * 4);
  for (size_t i = 0; i < t->color.size(); ++i) {
    const uint32_t c = t->color[i];
    out[i * 4 + 0] = (uint8_t)(c >> 16);
    out[i * 4 + 1] = (uint8_t)(c >> 8);
    out[i * 4 + 2] = (uint8_t)c;
    out[i * 4 + 3] = 0xFF;
  }
  return Value::bytes(std::move(out));
}

Value bi_r3d_stats(Vm&, std::vector<Value>& args, std::string& error) {
  auto t = get_target(args[0], error, "r3d_stats");
  if (!t) return Value::nil();
  auto m = std::make_shared<MapObj>();
  m->put("tris", Value::integer(t->tris));
  m->put("filled", Value::integer(t->filled));
  m->put("clipped", Value::integer(t->clipped));
  m->put("pixels", Value::integer(t->pixels));
  return Value::object(Type::Map, m);
}

// ---- the textured fill -----------------------------------------------------------------------------------------
//
//   r3d_texture(t, id, w, h, indices, palette[, mask])  -> id
//       a w x h picture of palette indices (bytes, row by row), its colours from palette (256 RGB triples, 768
//       bytes); with mask (bytes, w x h), the texels it marks 0 are transparent. Kept with mipmaps.
//   r3d_draw_tex(t, rows, fov_deg[, camera[, things]])   -> how many triangles were filled; with camera
//       {x, y, z, angle[, pitch]} the rows are world points on Doom's map axes (x east, y north, z up), turned here
//       (pitch: degrees looking up, negative down).
//       things (with a camera): maps {set, x, y, z, angle, light, flags} - a sprite frame (r3d_sprite_set) where
//       a thing stands (z its feet), facing angle (degrees), lit light (0..255), flags 2 full bright, 8 fuzz: its
//       picture upright facing the camera, the view of the eight for where the camera is. Turned here each frame,
//       so a thing faces the eye as it is now; drawn with the rows, in the same passes.
//       rows: maps with x0 y0 z0 u0 v0 .. x2 y2 z2 u2 v2 (camera space; u v in texels), tex (an id given to
//       r3d_texture), light (0..255) and flags: 1 masked (transparent texels let through what is behind), 2 full
//       bright (no light, no falloff), 4 clamp v (the texture is not repeated up or down: a masked middle texture),
//       8 fuzz (a spectre: what is behind darkened where the picture is opaque).
//   r3d_sky(t, tex, yaw_deg, fov_deg)                    -> t: the sky, Doom's way (the texture four times round the
//       horizon, its rows down from the top of the view), wherever nothing has been drawn: call it after r3d_draw_tex
//   r3d_blit(t, tex, x, y, w, h, light)                  -> t: the picture scaled to x, y, w, h on top of
//       everything (a weapon), transparent texels skipped
//   r3d_options(t, {bilinear, mipmaps, upscale, y_scale, light_gain, light_lift, light_fall}) -> t
//       upscale (1, 2, 4): what the pictures r3d_texture is given from then on are enlarged by, with a pixel-art
//       scaler (edges that run on a slant are followed, not stepped); their texel coordinates stay the picture's own
//   r3d_clear_depth(t)                                   -> t, the depth emptied and the colour kept

std::shared_ptr<R3dTex> tex_of(const R3dTarget& t, int64_t id) {
  return id >= 0 && (size_t)id < t.tex.size() ? t.tex[(size_t)id] : nullptr;
}

// The next mip level: each texel the mean of the 2x2 it covers (the texture repeats, so the last odd column or
// row wraps), colours averaged over the opaque texels only so transparent ones do not darken an edge.
R3dTex::Level half_of(const R3dTex::Level& a) {
  R3dTex::Level b;
  b.w = std::max(1, a.w / 2);
  b.h = std::max(1, a.h / 2);
  b.px.resize((size_t)b.w * b.h);
  for (int y = 0; y < b.h; ++y)
    for (int x = 0; x < b.w; ++x) {
      uint32_t r = 0, g = 0, bl = 0, al = 0, n = 0;
      for (int k = 0; k < 4; ++k) {
        const int sx = (x * 2 + (k & 1)) % a.w, sy = (y * 2 + (k >> 1)) % a.h;
        const uint32_t p = a.px[(size_t)sy * a.w + sx];
        const uint32_t pa = p >> 24;
        al += pa;
        if (pa) { r += (p >> 16) & 255; g += (p >> 8) & 255; bl += p & 255; ++n; }
      }
      const uint32_t A = al / 4;
      b.px[(size_t)y * b.w + x] = n ? ((A << 24) | ((r / n) << 16) | ((g / n) << 8) | (bl / n)) : 0;
    }
  return b;
}

// The pixel-art scaler: a picture twice the size, each texel four, where a corner of the four takes its neighbour's
// colour when an edge runs across that corner on a slant (Scale2x's rule - the two neighbours that meet at the
// corner alike, the two across from them not) - so a staircase of texels becomes a straight slanted edge, and
// everything else stays as it was drawn. "Alike" is close in brightness and hue (hq2x's limits: dithered texels
// of one surface count as one colour), transparent alike with transparent only. Tiling pictures wrap at their
// edges; one with transparent texels (a sprite) has nothing beyond its edge.
namespace {
inline bool alike(uint32_t a, uint32_t b) {
  const bool ta = (a >> 24) < 128, tb = (b >> 24) < 128;
  if (ta || tb) return ta == tb;
  const int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
  const int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
  const int y = std::abs((299 * (ar - br) + 587 * (ag - bg) + 114 * (ab - bb)) / 1000);
  const int u = std::abs((-169 * (ar - br) - 331 * (ag - bg) + 500 * (ab - bb)) / 1000);
  const int v = std::abs((500 * (ar - br) - 419 * (ag - bg) - 81 * (ab - bb)) / 1000);
  return y <= 48 && u <= 7 && v <= 6;
}
}  // namespace

R3dTex::Level scale2x(const R3dTex::Level& a, bool wrap) {
  R3dTex::Level b;
  b.w = a.w * 2;
  b.h = a.h * 2;
  b.px.resize((size_t)b.w * b.h);
  auto at = [&](int x, int y) -> uint32_t {
    if (wrap) { x = ((x % a.w) + a.w) % a.w; y = ((y % a.h) + a.h) % a.h; }
    else if (x < 0 || y < 0 || x >= a.w || y >= a.h) return 0;
    return a.px[(size_t)y * a.w + x];
  };
  for (int y = 0; y < a.h; ++y)
    for (int x = 0; x < a.w; ++x) {
      const uint32_t P = at(x, y), A = at(x, y - 1), B = at(x + 1, y), C = at(x - 1, y), D = at(x, y + 1);
      uint32_t e0 = P, e1 = P, e2 = P, e3 = P;
      if (!alike(A, D) && !alike(C, B)) {
        if (alike(C, A)) e0 = A;
        if (alike(A, B)) e1 = B;
        if (alike(D, C)) e2 = C;
        if (alike(B, D)) e3 = D;
      }
      uint32_t* o = &b.px[(size_t)(y * 2) * b.w + x * 2];
      o[0] = e0; o[1] = e1;
      o[b.w] = e2; o[b.w + 1] = e3;
    }
  return b;
}

Value bi_r3d_texture(Vm&, std::vector<Value>& args, std::string& error) {
  auto t = get_target(args[0], error, "r3d_texture");
  if (!t) return Value::nil();
  for (int i = 1; i <= 3; ++i) if (!want_number(args[(size_t)i], error, "r3d_texture")) return Value::nil();
  const int64_t id = args[1].as_int(), w = args[2].as_int(), h = args[3].as_int();
  if (id < 0 || id > 1000000) { error = "r3d_texture(): id out of range (0..1000000)"; return Value::nil(); }
  if (w < 1 || h < 1 || w > 4096 || h > 4096) { error = "r3d_texture(): size out of range (1..4096)"; return Value::nil(); }
  auto bytes_of = [&](const Value& v, const char* what, std::string_view* out) {
    if (v.type == Type::Bytes) { *out = std::string_view((const char*)v.bytes_data().data(), v.bytes_data().size()); return true; }
    if (v.type == Type::Str) { *out = v.str(); return true; }
    error = std::string("r3d_texture(): ") + what + " wants bytes or a string, got a " + type_name(v.type);
    return false;
  };
  std::string_view idx, pal, mask;
  if (!bytes_of(args[4], "indices", &idx) || !bytes_of(args[5], "palette", &pal)) return Value::nil();
  const bool has_mask = args.size() > 6 && args[6].type != Type::Nil;
  if (has_mask && !bytes_of(args[6], "mask", &mask)) return Value::nil();
  const size_t n = (size_t)w * (size_t)h;
  if (idx.size() < n) { error = "r3d_texture(): " + std::to_string(idx.size()) + " indices for a " + std::to_string(w) + "x" + std::to_string(h) + " picture"; return Value::nil(); }
  if (pal.size() < 768) { error = "r3d_texture(): the palette wants 768 bytes (256 RGB), got " + std::to_string(pal.size()); return Value::nil(); }
  if (has_mask && mask.size() < n) { error = "r3d_texture(): the mask is shorter than the picture"; return Value::nil(); }
  auto tx = std::make_shared<R3dTex>();
  R3dTex::Level L0;
  L0.w = (int)w;
  L0.h = (int)h;
  L0.px.resize(n);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t k = (uint8_t)idx[i];
    const bool opaque = !has_mask || mask[i] != 0;
    if (!opaque) tx->alpha = true;
    L0.px[i] = opaque ? (0xFF000000u | ((uint32_t)(uint8_t)pal[k * 3] << 16) | ((uint32_t)(uint8_t)pal[k * 3 + 1] << 8) |
                         (uint32_t)(uint8_t)pal[k * 3 + 2])
                      : 0u;
  }
  for (int s2 = 1; s2 < t->upscale; s2 *= 2) {
    L0 = scale2x(L0, !tx->alpha);
    tx->scale *= 2;
  }
  tx->mips.push_back(std::move(L0));
  while (tx->mips.back().w > 1 || tx->mips.back().h > 1) tx->mips.push_back(half_of(tx->mips.back()));
  if ((size_t)id >= t->tex.size()) t->tex.resize((size_t)id + 1);
  t->tex[(size_t)id] = std::move(tx);
  return Value::integer(id);
}

// A texel of level L at (u, v) in that level's texels, as 0xAARRGGBB; wrap_v false: rows outside the picture are
// transparent.
inline uint32_t texel(const R3dTex::Level& L, int u, int v, bool wrap_v) {
  u %= L.w;
  if (u < 0) u += L.w;
  if (wrap_v) { v %= L.h; if (v < 0) v += L.h; }
  else if (v < 0 || v >= L.h) return 0;
  return L.px[(size_t)v * L.w + u];
}

inline uint32_t sample(const R3dTex::Level& L, double u, double v, bool wrap_v, bool bilinear) {
  if (!bilinear) return texel(L, (int)std::floor(u), (int)std::floor(v), wrap_v);
  const double fu = u - 0.5, fv = v - 0.5;
  const int x0 = (int)std::floor(fu), y0 = (int)std::floor(fv);
  const uint32_t ax = (uint32_t)((fu - x0) * 256.0), ay = (uint32_t)((fv - y0) * 256.0);
  // the four texels as texel() has them, with one wrap of each coordinate rather than one per texel: an integer
  // divide is 26-90 cycles on x86 (a few on Apple's cores), and this runs for every pixel of every textured surface
  // a power-of-two size (Doom's walls and flats all but always) wraps with a mask: the same as the divide's
  // remainder made positive, two's complement making x & (w - 1) that for negative x too
  int xa;
  if ((L.w & (L.w - 1)) == 0) xa = x0 & (L.w - 1);
  else { xa = x0 % L.w; if (xa < 0) xa += L.w; }
  const int xb = xa + 1 == L.w ? 0 : xa + 1;
  uint32_t p00, p10, p01, p11;
  if (wrap_v) {
    int ya;
    if ((L.h & (L.h - 1)) == 0) ya = y0 & (L.h - 1);
    else { ya = y0 % L.h; if (ya < 0) ya += L.h; }
    const int yb = ya + 1 == L.h ? 0 : ya + 1;
    const uint32_t* ra = &L.px[(size_t)ya * L.w];
    const uint32_t* rb = &L.px[(size_t)yb * L.w];
    p00 = ra[xa]; p10 = ra[xb]; p01 = rb[xa]; p11 = rb[xb];
  } else {
    const bool ina = y0 >= 0 && y0 < L.h, inb = y0 + 1 >= 0 && y0 + 1 < L.h;
    const uint32_t* ra = ina ? &L.px[(size_t)y0 * L.w] : nullptr;
    const uint32_t* rb = inb ? &L.px[(size_t)(y0 + 1) * L.w] : nullptr;
    p00 = ra ? ra[xa] : 0; p10 = ra ? ra[xb] : 0; p01 = rb ? rb[xa] : 0; p11 = rb ? rb[xb] : 0;
  }
  // weights of the four, and each channel weighted by its texel's alpha (so a transparent neighbour does not
  // bleed its black in): colour = sum(c a w) / sum(a w), alpha = sum(a w)
  const uint32_t w00 = (256 - ax) * (256 - ay), w10 = ax * (256 - ay), w01 = (256 - ax) * ay, w11 = ax * ay;
  // all four opaque (a wall's or a flat's texels): the weights sum to 65536 and every alpha is 255, so
  // sum(c a w) / sum(a w) is sum(c w) >> 16 exactly - no divides
  if ((p00 & p10 & p01 & p11) >> 24 == 255) {
    auto ch = [&](int sh) {
      return (((p00 >> sh) & 255) * w00 + ((p10 >> sh) & 255) * w10 + ((p01 >> sh) & 255) * w01 + ((p11 >> sh) & 255) * w11) >> 16;
    };
    return 0xFF000000u | (ch(16) << 16) | (ch(8) << 8) | ch(0);
  }
  uint64_t A = 0, R = 0, G = 0, B = 0;
  auto add = [&](uint32_t p, uint32_t w) {
    const uint64_t a = (uint64_t)(p >> 24) * w;
    A += a;
    R += a * ((p >> 16) & 255); G += a * ((p >> 8) & 255); B += a * (p & 255);
  };
  add(p00, w00); add(p10, w10); add(p01, w01); add(p11, w11);
  if (A == 0) return 0;
  const uint32_t alpha = (uint32_t)(A >> 16);
  return (std::min<uint32_t>(alpha, 255) << 24) | ((uint32_t)(R / A) << 16) | ((uint32_t)(G / A) << 8) | (uint32_t)(B / A);
}

// Noise, for a sculpt's relief and colour and the fill's fine relief.
double hash3(int x, int y, int z) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)z * 2147483647u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return ((h ^ (h >> 16)) & 0xFFFFFF) / 16777215.0;
}
// smooth value noise in [0, 1]
double vnoise(double x, double y, double z) {
  const int xi = (int)std::floor(x), yi = (int)std::floor(y), zi = (int)std::floor(z);
  const double fx = x - xi, fy = y - yi, fz = z - zi;
  auto sm = [](double f) { return f * f * (3 - 2 * f); };
  const double u = sm(fx), v = sm(fy), w = sm(fz);
  double acc = 0;
  for (int k = 0; k < 8; ++k) {
    const int dx = k & 1, dy = (k >> 1) & 1, dz = k >> 2;
    acc += hash3(xi + dx, yi + dy, zi + dz) * (dx ? u : 1 - u) * (dy ? v : 1 - v) * (dz ? w : 1 - w);
  }
  return acc;
}
// three octaves: broad lumps, then folds, then grain
double fbm(double x, double y, double z) {
  return vnoise(x, y, z) * 0.55 + vnoise(x * 2.03 + 11.1, y * 2.03, z * 2.03) * 0.30 + vnoise(x * 4.1, y * 4.1 + 7.7, z * 4.1) * 0.15;
}
constexpr int kAttrs = 9;   // a mesh vertex's: r g b, nx ny nz, ox oy oz

// One triangle in screen space with what the fill interpolates: 1/z, u/z and v/z are linear on the screen.
struct STri {
  int x0, x1, y0, y1;            // bounding box, clipped to the target
  double ea[3], eb[3], ec[3];    // edge functions, positive inside
  double qx, qy, qc;             // 1/z = qx x + qy y + qc
  double ux, uy, uc, vx, vy, vc; // u/z, v/z likewise
  // a mesh's triangle (tex null): its attributes over z, each linear on the screen as u/z is - colour r g b, normal
  // nx ny nz (camera space), the surface's own point ox oy oz (where it is on the model, for its fine relief)
  double ap[kAttrs][3];
  float shine = 0, glow = 0;
  const R3dTex* tex;
  float light;                   // 0..1
  int flags;
  // the lights that can reach it (indices into the target's lights_cam), and its face's normal in camera space
  uint8_t nl = 0;
  uint8_t li[8];
  float fnx = 0, fny = 0, fnz = -1;
};

// What the lights in reach add at a point of a surface (camera space) with normal n: per channel, as a share of the
// surface's own colour. Each falls off smoothly to nothing at its radius, and lights only what faces it.
inline void dyn_light(const R3dTarget& t, const STri& s, double px, double py, double pz, double nx, double ny, double nz,
                      double* dr, double* dg, double* db) {
  for (int i = 0; i < s.nl; ++i) {
    const R3dLight& L = t.lights_cam[s.li[i]];
    const double dx = L.x - px, dy = L.y - py, dz = L.z - pz;
    const double d2 = dx * dx + dy * dy + dz * dz, r2 = L.radius * L.radius;
    if (d2 >= r2) continue;
    const double inv = 1.0 / std::sqrt(std::max(1e-6, d2));
    const double ndl = (nx * dx + ny * dy + nz * dz) * inv;
    if (ndl <= 0) continue;
    const double f = 1.0 - d2 / r2;
    const double k = f * f * (0.35 + 0.65 * ndl);
    *dr += L.r * k; *dg += L.g * k; *db += L.b * k;
  }
}

// A vertex: where it is (camera space), its texel, and - a mesh's, drawn without a texture - its lit colour (0..1+).
struct TV {
  double x, y, z, u, v;
  double a[kAttrs] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
  float shine = 0, glow = 0;
};

// The screen triangle of three projected points (sx, sy, 1/z, u/z, v/z); false when it covers nothing.
bool setup(const R3dTarget& t, const double* sx, const double* sy, const double* q, const double* uq, const double* vq, STri* o,
           const double (*aq)[3] = nullptr) {
  double area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
  if (!(std::fabs(area) > 1e-9)) return false;
  const double sgn = area > 0 ? 1.0 : -1.0;
  area = std::fabs(area);
  int x0 = (int)std::floor(std::min({sx[0], sx[1], sx[2]})), x1 = (int)std::ceil(std::max({sx[0], sx[1], sx[2]}));
  int y0 = (int)std::floor(std::min({sy[0], sy[1], sy[2]})), y1 = (int)std::ceil(std::max({sy[0], sy[1], sy[2]}));
  o->x0 = std::max(x0, 0); o->y0 = std::max(y0, 0);
  o->x1 = std::min(x1, t.w - 1); o->y1 = std::min(y1, t.h - 1);
  if (o->x0 > o->x1 || o->y0 > o->y1) return false;
  for (int i = 0; i < 3; ++i) {
    const int j = (i + 1) % 3;
    o->ea[i] = sgn * (sy[i] - sy[j]);
    o->eb[i] = sgn * (sx[j] - sx[i]);
    o->ec[i] = sgn * (sx[i] * sy[j] - sx[j] * sy[i]);
  }
  // an attribute a is (e1 a0 + e2 a1 + e0 a2) / area at a pixel (vertex 2 by edge 0, vertex 0 by edge 1, vertex
  // 1 by edge 2): its x, y and constant coefficients
  auto plane = [&](const double* a, double* cx, double* cy, double* cc) {
    *cx = (o->ea[1] * a[0] + o->ea[2] * a[1] + o->ea[0] * a[2]) / area;
    *cy = (o->eb[1] * a[0] + o->eb[2] * a[1] + o->eb[0] * a[2]) / area;
    *cc = (o->ec[1] * a[0] + o->ec[2] * a[1] + o->ec[0] * a[2]) / area;
  };
  plane(q, &o->qx, &o->qy, &o->qc);
  plane(uq, &o->ux, &o->uy, &o->uc);
  plane(vq, &o->vx, &o->vy, &o->vc);
  if (aq)
    for (int k = 0; k < kAttrs; ++k) plane(aq[k], &o->ap[k][0], &o->ap[k][1], &o->ap[k][2]);
  return true;
}

// Fill rows [ya, yb) of a mesh's triangle, lit per pixel: its colour under a key light from above the eye's left
// shoulder and a cool fill from behind, light scattered under the skin past the shadow's edge, a highlight as
// tight as the surface is polished, a rim where it turns from the eye, fine relief from the surface's own point
// (so it stays on the creature as it moves) - then the sector's light and its fall with distance as for every
// surface (not for full bright or what glows). Fuzz: what is behind it, bent and darkened.
void fill_col_rows(R3dTarget& t, const STri& s, int ya, int yb, int64_t* pixels) {
  const bool bright = (s.flags & 2) != 0, fuzz = (s.flags & 8) != 0;
  const float L = s.light;
  const double gain = t.light_gain, lift = t.light_lift, fall = t.light_fall;
  const double cx = t.w * 0.5, cy = t.h * 0.5, focal = t.focal, fy = t.focal * t.y_scale;
  // camera space: x right, y up, z ahead; each the direction toward the light
  static const double kl[3] = {-0.4170, 0.6157, -0.6686}, fl[3] = {0.5, -0.35, 0.79};
  const double shine = s.shine, glow = s.glow;
  const double spec_pow = 10.0 + 70.0 * shine;
  const int y_lo = std::max(s.y0, ya), y_hi = std::min(s.y1, yb - 1);
  for (int y = y_lo; y <= y_hi; ++y) {
    const double py = y + 0.5;
    double lo = s.x0 + 0.5, hi = s.x1 + 0.5;
    double c[3];
    bool empty = false;
    for (int i = 0; i < 3 && !empty; ++i) {
      c[i] = s.eb[i] * py + s.ec[i];
      if (s.ea[i] > 0) lo = std::max(lo, -c[i] / s.ea[i]);
      else if (s.ea[i] < 0) hi = std::min(hi, -c[i] / s.ea[i]);
      else if (c[i] < 0) empty = true;
    }
    if (empty || lo > hi) continue;
    const int xa = std::max(s.x0, (int)std::floor(lo - 0.5) - 1), xb = std::min(s.x1, (int)std::ceil(hi - 0.5) + 1);
    float* drow = &t.depth[(size_t)y * t.w];
    uint32_t* crow = &t.color[(size_t)y * t.w];
    const double px0 = xa + 0.5;
    double e0 = s.ea[0] * px0 + c[0], e1 = s.ea[1] * px0 + c[1], e2 = s.ea[2] * px0 + c[2];
    double q = s.qx * px0 + s.qy * py + s.qc;
    double A[kAttrs];
    for (int k = 0; k < kAttrs; ++k) A[k] = s.ap[k][0] * px0 + s.ap[k][1] * py + s.ap[k][2];
    for (int x = xa; x <= xb; ++x, e0 += s.ea[0], e1 += s.ea[1], e2 += s.ea[2], q += s.qx) {
      if (x > xa) for (int k = 0; k < kAttrs; ++k) A[k] += s.ap[k][0];
      if (e0 < 0 || e1 < 0 || e2 < 0 || q <= 0) continue;
      const float qf = (float)q;
      if (qf <= drow[x]) continue;
      if (fuzz) {
        const int sx2 = std::min(t.w - 1, std::max(0, x + (int)(std::sin(y * 0.21 + t.time * 7.0) * 3.0)));
        const uint32_t d = crow[sx2];
        const uint32_t k = 110 + (uint32_t)(((x * 7 + y * 13 + (int)(t.time * 40)) ^ (x >> 2)) & 63);
        crow[x] = ((((d >> 16) & 255) * k >> 8) << 16) | ((((d >> 8) & 255) * k >> 8) << 8) | (((d & 255) * k) >> 8);
        continue;
      }
      const double z = 1.0 / q;
      double nx = A[3] * z, ny = A[4] * z, nz = A[5] * z;
      // the smooth normal, for the dynamic lights: relief that only shades the key light would turn to blotches
      // under a strong light close by
      double sn = 1.0 / std::sqrt(std::max(1e-12, nx * nx + ny * ny + nz * nz));
      const double snx = nx * sn, sny = ny * sn, snz = nz * sn;
      // fine relief: the noise's slope at the surface's own point tips the normal (less on what is polished, none
      // on a mirror; a textured model's own picture has its detail)
      if (!s.tex && shine < 1.0) {
        const double ox = A[6] * z * 1.3, oy = A[7] * z * 1.3, oz = A[8] * z * 1.3, e = 0.35;
        const double n0 = vnoise(ox, oy, oz);
        const double gx = vnoise(ox + e, oy, oz) - n0, gy = vnoise(ox, oy + e, oz) - n0, gz = vnoise(ox, oy, oz + e) - n0;
        const double amp = 1.5 * (1.0 - 0.6 * shine) * std::min(1.0, 2.0 * (1.0 - shine));
        nx -= gx * amp; ny -= gz * amp; nz -= gy * amp;
      }
      const double nl = 1.0 / std::sqrt(std::max(1e-12, nx * nx + ny * ny + nz * nz));
      nx *= nl; ny *= nl; nz *= nl;
      const double cxp = (x + 0.5 - cx) / focal * z, cyp = -(py - cy) / fy * z;
      const double vl = 1.0 / std::sqrt(cxp * cxp + cyp * cyp + z * z);
      const double vx = -cxp * vl, vy = -cyp * vl, vz = -z * vl;
      const double ndl = nx * kl[0] + ny * kl[1] + nz * kl[2];
      const double lam = std::max(0.0, ndl);
      const double wrap = std::max(0.0, (ndl + 0.45) / 1.45);
      const double fill = std::max(0.0, nx * fl[0] + ny * fl[1] + nz * fl[2]) * 0.18;
      const double ndv = std::max(0.0, nx * vx + ny * vy + nz * vz);
      const double hx = kl[0] + vx, hy = kl[1] + vy, hz = kl[2] + vz;
      const double hl = 1.0 / std::sqrt(std::max(1e-12, hx * hx + hy * hy + hz * hz));
      const double ndh = std::max(0.0, (nx * hx + ny * hy + nz * hz) * hl);
      const double spec = shine * 1.1 * std::pow(ndh, spec_pow) * (0.25 + 0.75 * lam);
      const double rim = 0.30 * std::pow(1.0 - ndv, 4.0);
      const double scat = (wrap - lam) * 0.55;
      double ar = A[0] * z, ag = A[1] * z, ab = A[2] * z;
      if (s.tex) {
        const R3dTex& tx = *s.tex;
        const double u = (s.ux * (x + 0.5) + s.uy * py + s.uc) * z, v = (s.vx * (x + 0.5) + s.vy * py + s.vc) * z;
        int lv = 0;
        if (t.mipmaps && tx.mips.size() > 1) {
          const double dux = (s.ux - u * s.qx) * z, dvx = (s.vx - v * s.qx) * z, duy = (s.uy - u * s.qy) * z, dvy = (s.vy - v * s.qy) * z;
          const double rho2 = std::sqrt((dux * dux + dvx * dvx) * (duy * duy + dvy * dvy));
          if (rho2 > 1.0) { int e = 0; std::frexp(rho2, &e); lv = std::min((int)tx.mips.size() - 1, (e - 1) / 2); }
        }
        const R3dTex::Level& LV = tx.mips[(size_t)lv];
        const uint32_t p = sample(LV, u * LV.w / tx.mips[0].w, v * LV.h / tx.mips[0].h, true, t.bilinear);
        if ((p >> 24) < 128) continue;
        ar *= ((p >> 16) & 255) / 255.0; ag *= ((p >> 8) & 255) / 255.0; ab *= (p & 255) / 255.0;
      }
      const double amb = 0.16;
      const double r = ar * (amb + lam * 0.95 + fill + scat * 1.0 + glow * 2.2) + spec + rim * 0.9;
      const double g = ag * (amb + lam * 0.95 + fill + scat * 0.45 + glow * 2.2) + spec + rim * 0.95;
      const double bl = ab * (amb + lam * 0.95 + fill * 1.3 + scat * 0.30 + glow * 2.2) + spec + rim * 1.1;
      double b = 255.0;
      if (!bright && glow < 0.5) {
        const double cmap = std::min(31.0, std::max(0.0, 60.0 - L * 63.75 - fall / std::max(z, 1.0)));
        b *= std::min(1.0, std::max(0.0, 1.0 - cmap / 32.0 * gain + lift));
      }
      double dr = 0, dg = 0, db = 0;
      if (s.nl && !bright) dyn_light(t, s, cxp, cyp, z, snx, sny, snz, &dr, &dg, &db);
      crow[x] = ((uint32_t)std::min(255.0, std::max(0.0, r * b + ar * dr * 255.0)) << 16) |
                ((uint32_t)std::min(255.0, std::max(0.0, g * b + ag * dg * 255.0)) << 8) |
                (uint32_t)std::min(255.0, std::max(0.0, bl * b + ab * db * 255.0));
      drow[x] = qf;
      ++*pixels;
    }
  }
}

// Fill rows [ya, yb) of one triangle.
void fill_tex_rows(R3dTarget& t, const STri& s, int ya, int yb, int64_t* pixels) {
  if (!s.tex || (s.flags & 64)) { fill_col_rows(t, s, ya, yb, pixels); return; }
  const R3dTex* tx = s.tex;
  const bool masked = (s.flags & 1) != 0, bright = (s.flags & 2) != 0, wrap_v = (s.flags & 4) == 0, fuzz = (s.flags & 8) != 0;
  const int levels = (int)tx->mips.size();
  // u, v are in the picture's own texels; level 0 has `scale` of its texels to each
  const double W0 = (double)tx->mips[0].w / tx->scale, H0 = (double)tx->mips[0].h / tx->scale;
  const double rho_scale = (double)tx->scale * tx->scale;
  const float L = s.light;
  const double gain = t.light_gain, lift = t.light_lift, fall = t.light_fall;
  const bool bil = t.bilinear, mips = t.mipmaps;
  const double lcx = t.w * 0.5, lcy = t.h * 0.5, lfy = t.focal * t.y_scale;
  const int y_lo = std::max(s.y0, ya), y_hi = std::min(s.y1, yb - 1);
  for (int y = y_lo; y <= y_hi; ++y) {
    const double py = y + 0.5;
    double lo = s.x0 + 0.5, hi = s.x1 + 0.5;
    double c[3];
    bool empty = false;
    for (int i = 0; i < 3 && !empty; ++i) {
      c[i] = s.eb[i] * py + s.ec[i];
      if (s.ea[i] > 0) lo = std::max(lo, -c[i] / s.ea[i]);
      else if (s.ea[i] < 0) hi = std::min(hi, -c[i] / s.ea[i]);
      else if (c[i] < 0) empty = true;
    }
    if (empty || lo > hi) continue;
    const int xa = std::max(s.x0, (int)std::floor(lo - 0.5) - 1), xb = std::min(s.x1, (int)std::ceil(hi - 0.5) + 1);
    float* drow = &t.depth[(size_t)y * t.w];
    uint32_t* crow = &t.color[(size_t)y * t.w];
    double px = xa + 0.5;
    double e0 = s.ea[0] * px + c[0], e1 = s.ea[1] * px + c[1], e2 = s.ea[2] * px + c[2];
    double q = s.qx * px + s.qy * py + s.qc, uq = s.ux * px + s.uy * py + s.uc, vq = s.vx * px + s.vy * py + s.vc;
    for (int x = xa; x <= xb; ++x, e0 += s.ea[0], e1 += s.ea[1], e2 += s.ea[2], q += s.qx, uq += s.ux, vq += s.vx) {
      if (e0 < 0 || e1 < 0 || e2 < 0 || q <= 0) continue;
      const float qf = (float)q;
      if (qf <= drow[x]) continue;
      const double z = 1.0 / q;
      const double u = uq * z, v = vq * z;
      // the mip level: log2 of how many texels one pixel steps over (the larger of its two screen directions)
      int lv = 0;
      if (mips && levels > 1) {
        const double dux = (s.ux - u * s.qx) * z, dvx = (s.vx - v * s.qx) * z;
        const double duy = (s.uy - u * s.qy) * z, dvy = (s.vy - v * s.qy) * z;
        // the geometric mean of the two directions' steps: a floor seen at a slant steps far along one and little
        // along the other, and the larger alone would blur it to one colour (the cost of not filtering it
        // anisotropically)
        const double rho2 = std::sqrt((dux * dux + dvx * dvx) * (duy * duy + dvy * dvy)) * rho_scale;
        if (rho2 > 1.0) {
          int e = 0;
          std::frexp(rho2, &e);   // rho2 in [2^(e-1), 2^e): log2(rho) about (e - 1) / 2
          lv = std::min(levels - 1, (e - 1) / 2);
        }
      }
      const R3dTex::Level& LV = tx->mips[(size_t)lv];
      const uint32_t p = sample(LV, u * LV.w / W0, v * LV.h / H0, wrap_v, bil);
      const uint32_t a = p >> 24;
      if (masked || fuzz) { if (a < 128) continue; }
      if (fuzz) {   // what is behind, darkened; the depth is left alone so the scene stays visible through it
        const uint32_t d = crow[x];
        const uint32_t k = 140 + (uint32_t)(((x * 7 + y * 13) ^ (x >> 2)) & 31);
        crow[x] = ((((d >> 16) & 255) * k >> 8) << 16) | ((((d >> 8) & 255) * k >> 8) << 8) | (((d & 255) * k) >> 8);
        continue;
      }
      double b = 1.0;
      if (!bright) {
        const double cmap = std::min(31.0, std::max(0.0, 60.0 - L * 63.75 - fall / std::max(z, 1.0)));
        b *= std::min(1.0, std::max(0.0, 1.0 - cmap / 32.0 * gain + lift));
      }
      double br = b, bg = b, bb = b;
      if (s.nl && !bright)
        dyn_light(t, s, (x + 0.5 - lcx) / t.focal * z, (lcy - py) / lfy * z, z, s.fnx, s.fny, s.fnz, &br, &bg, &bb);
      const uint32_t r = (uint32_t)std::min(255.0, ((p >> 16) & 255) * br), g = (uint32_t)std::min(255.0, ((p >> 8) & 255) * bg),
                     bl = (uint32_t)std::min(255.0, (p & 255) * bb);
      crow[x] = (r << 16) | (g << 8) | bl;
      drow[x] = qf;
      ++*pixels;
    }
  }
}

// Near-plane clip of a textured triangle (u, v carried along, linear in camera space), projected into STris.
void clip_project(const R3dTarget& t, const TV* v, double focal, const R3dTex* tx, float light, int flags, std::vector<STri>* out, int64_t* clipped) {
  const bool in[3] = {v[0].z >= kNear, v[1].z >= kNear, v[2].z >= kNear};
  const int nin = (int)in[0] + (int)in[1] + (int)in[2];
  if (nin == 0) { ++*clipped; return; }
  TV p[4];
  int n = 0;
  if (nin == 3) { p[0] = v[0]; p[1] = v[1]; p[2] = v[2]; n = 3; }
  else {
    ++*clipped;
    for (int i = 0; i < 3; ++i) {
      const int j = (i + 1) % 3;
      if (in[i]) p[n++] = v[i];
      if (in[i] != in[j]) {
        const double k = (kNear - v[i].z) / (v[j].z - v[i].z);
        TV m{v[i].x + (v[j].x - v[i].x) * k, v[i].y + (v[j].y - v[i].y) * k, kNear,
             v[i].u + (v[j].u - v[i].u) * k, v[i].v + (v[j].v - v[i].v) * k};
        for (int e = 0; e < kAttrs; ++e) m.a[e] = v[i].a[e] + (v[j].a[e] - v[i].a[e]) * k;
        m.shine = v[i].shine; m.glow = v[i].glow;
        p[n++] = m;
      }
    }
  }
  // the face's normal (camera space, turned toward the eye) and the lights whose reach meets its box and that are
  // in front of it
  float fn[3] = {0, 0, -1};
  uint8_t li[8];
  uint8_t nl = 0;
  if (!t.lights_cam.empty() && !(flags & 2)) {
    const double ax = p[1].x - p[0].x, ay = p[1].y - p[0].y, az = p[1].z - p[0].z;
    const double bx = p[2].x - p[0].x, by = p[2].y - p[0].y, bz = p[2].z - p[0].z;
    double nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
    const double nn = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (nn > 1e-12) {
      nx /= nn; ny /= nn; nz /= nn;
      if (nx * p[0].x + ny * p[0].y + nz * p[0].z > 0) { nx = -nx; ny = -ny; nz = -nz; }
      fn[0] = (float)nx; fn[1] = (float)ny; fn[2] = (float)nz;
    }
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (int i = 0; i < n; ++i) {
      lo[0] = std::min(lo[0], p[i].x); hi[0] = std::max(hi[0], p[i].x);
      lo[1] = std::min(lo[1], p[i].y); hi[1] = std::max(hi[1], p[i].y);
      lo[2] = std::min(lo[2], p[i].z); hi[2] = std::max(hi[2], p[i].z);
    }
    const bool mesh = !tx || (flags & 64);   // a model's own normals vary across a face: no facing test for it
    for (size_t k = 0; k < t.lights_cam.size() && nl < 8; ++k) {
      const R3dLight& L = t.lights_cam[k];
      const double ex = std::max(0.0, std::max(lo[0] - L.x, L.x - hi[0]));
      const double ey = std::max(0.0, std::max(lo[1] - L.y, L.y - hi[1]));
      const double ez = std::max(0.0, std::max(lo[2] - L.z, L.z - hi[2]));
      if (ex * ex + ey * ey + ez * ez >= L.radius * L.radius) continue;
      if (!mesh && fn[0] * (L.x - p[0].x) + fn[1] * (L.y - p[0].y) + fn[2] * (L.z - p[0].z) <= 0) continue;
      li[nl++] = (uint8_t)k;
    }
  }
  double sx[4], sy[4], q[4], uq[4], vq[4], aqv[kAttrs][4];
  const double cx = t.w * 0.5, cy = t.h * 0.5, fy = focal * t.y_scale;
  for (int i = 0; i < n; ++i) {
    q[i] = 1.0 / p[i].z;
    sx[i] = cx + p[i].x * focal * q[i];
    sy[i] = cy - p[i].y * fy * q[i];
    uq[i] = p[i].u * q[i];
    vq[i] = p[i].v * q[i];
    for (int e = 0; e < kAttrs; ++e) aqv[e][i] = p[i].a[e] * q[i];
  }
  for (int k = 0; k + 2 < n; ++k) {   // (0 1 2), and for a quad (0 2 3)
    const int a = 0, b = k + 1, c = k + 2;
    const double tsx[3] = {sx[a], sx[b], sx[c]}, tsy[3] = {sy[a], sy[b], sy[c]}, tq[3] = {q[a], q[b], q[c]};
    const double tu[3] = {uq[a], uq[b], uq[c]}, tv[3] = {vq[a], vq[b], vq[c]};
    STri s;
    if (!tx || (flags & 64)) {
      double ta[kAttrs][3];
      for (int e = 0; e < kAttrs; ++e) { ta[e][0] = aqv[e][a]; ta[e][1] = aqv[e][b]; ta[e][2] = aqv[e][c]; }
      if (!setup(t, tsx, tsy, tq, tu, tv, &s, ta)) continue;
      s.shine = (p[a].shine + p[b].shine + p[c].shine) / 3.0f;
      s.glow = (p[a].glow + p[b].glow + p[c].glow) / 3.0f;
    } else if (!setup(t, tsx, tsy, tq, tu, tv, &s)) continue;
    s.tex = tx;
    s.light = light;
    s.flags = flags;
    s.nl = nl;
    for (int i = 0; i < nl; ++i) s.li[i] = li[i];
    s.fnx = fn[0]; s.fny = fn[1]; s.fnz = fn[2];
    out->push_back(s);
  }
}

// ---- meshes --------------------------------------------------------------------------------------------------------
//
//   r3d_mesh(t, id, parts, {rest, cell}) -> {verts, tris, ms}
//       a sculpted model: one continuous surface, the shapes in `parts` melted into each other where they meet
//       (a smooth union, each shape's `blend` how far its seam spreads), roughened by its `rough` (relief of that
//       many units, in patches of `grain`), built into a mesh at `cell` units of detail. Each shape is fixed to a
//       bone; each point of the surface is weighted between its two nearest bones, so it bends as flesh does
//       where two meet. rest: the bones' matrices at rest (12 numbers a bone, the model's space from each bone's,
//       as r3d_draw_tex's things give them posed). Shapes, in their bone's space (x forward, y left, z up):
//         {kind: "limb", bone, a: [x y z], b: [x y z], r0, r1}     a rounded cone from a (radius r0) to b (r1)
//         {kind: "ellipsoid", bone, c: [x y z], r: [rx ry rz], rot: [yaw pitch roll]?}
//         {kind: "box", bone, c: [x y z], r: [half sizes], rot?, round}   a box, its edges rounded by `round`
//       and each col (0xRRGGBB), col2 (a second colour it grades into: a limb from a to b, the rest bottom to top),
//       mottle (0..1, its colour's variation over the surface in patches of `grain` units), shine (0..1),
//       glow (it lights itself), blend (units; default 0.8), rough (units; default 0), grain (default 4), and
//       carve: true to cut the shape out of what is there instead of adding it (a mouth, an eye socket).

struct R3dMesh {
  struct V { float x, y, z, nx, ny, nz, r, g, b, shine, glow; uint8_t bone[2]; float w0; };
  std::vector<V> v;
  std::vector<uint32_t> idx;
  std::vector<double> inv_rest;   // per bone: the inverse of its rest matrix (12 numbers)
  int bones = 1;
  double radius = 0, height = 0;
};

namespace {
struct Vec { double x, y, z; };
Vec vadd(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec vsub(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec vmul(Vec a, double k) { return {a.x * k, a.y * k, a.z * k}; }
double vdot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double vlen(Vec a) { return std::sqrt(vdot(a, a)); }
Vec vcross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec vnorm(Vec a) { const double l = vlen(a); return l > 1e-12 ? vmul(a, 1.0 / l) : Vec{0, 0, 1}; }
// a 3x3 rotation, yaw about z then pitch about y then roll about x (degrees): its rows
void rot_rows(const double* ypr, double* R) {
  const double kD = 3.14159265358979323846 / 180.0;
  const double cy = std::cos(ypr[0] * kD), sy = std::sin(ypr[0] * kD), cp = std::cos(ypr[1] * kD), sp = std::sin(ypr[1] * kD);
  const double cr = std::cos(ypr[2] * kD), sr = std::sin(ypr[2] * kD);
  const double M[9] = {cy * cp, cy * sp * sr - sy * cr, cy * sp * cr + sy * sr,
                       sy * cp, sy * sp * sr + cy * cr, sy * sp * cr - cy * sr,
                       -sp, cp * sr, cp * cr};
  for (int i = 0; i < 9; ++i) R[i] = M[i];
}
Vec m34_point(const double* M, Vec p) {
  return {M[0] * p.x + M[1] * p.y + M[2] * p.z + M[3], M[4] * p.x + M[5] * p.y + M[6] * p.z + M[7], M[8] * p.x + M[9] * p.y + M[10] * p.z + M[11]};
}
Vec m34_dir(const double* M, Vec p) {
  return {M[0] * p.x + M[1] * p.y + M[2] * p.z, M[4] * p.x + M[5] * p.y + M[6] * p.z, M[8] * p.x + M[9] * p.y + M[10] * p.z};
}
// the inverse of a rigid 3x4 (rotation and translation)
void m34_inverse_rigid(const double* M, double* I) {
  I[0] = M[0]; I[1] = M[4]; I[2] = M[8];
  I[4] = M[1]; I[5] = M[5]; I[6] = M[9];
  I[8] = M[2]; I[9] = M[6]; I[10] = M[10];
  I[3] = -(I[0] * M[3] + I[1] * M[7] + I[2] * M[11]);
  I[7] = -(I[4] * M[3] + I[5] * M[7] + I[6] * M[11]);
  I[11] = -(I[8] * M[3] + I[9] * M[7] + I[10] * M[11]);
}
void m34_mul(const double* A, const double* B, double* O) {
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 4; ++c)
      O[r * 4 + c] = A[r * 4] * B[c] + A[r * 4 + 1] * B[4 + c] + A[r * 4 + 2] * B[8 + c] + (c == 3 ? A[r * 4 + 3] : 0.0);
  }
}
bool vec_field(MapObj* m, const char* k, double* out, int n, std::string& error, size_t part) {
  Value* v = m->find(k);
  if (!v) return false;
  if (v->type != Type::List || v->list_items().size() != (size_t)n) {
    error = "r3d_mesh(): part " + std::to_string(part) + "'s " + k + " wants a list of " + std::to_string(n) + " numbers";
    return false;
  }
  for (int i = 0; i < n; ++i) {
    const Value& e = v->list_items()[(size_t)i];
    if (!e.is_number()) { error = "r3d_mesh(): part " + std::to_string(part) + "'s " + k + " wants numbers"; return false; }
    out[i] = e.as_double();
  }
  return true;
}

// One shape of a sculpt, placed in the model's space at rest.
struct Shape {
  int kind = 0;               // 0 limb, 1 ellipsoid, 2 box
  int bone = 0;
  Vec a{0, 0, 0}, b{0, 0, 0}; // a limb's ends; an ellipsoid's or box's centre is a
  double r0 = 1, r1 = 1;
  double R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // model-space -> shape-space rotation (rows)
  Vec half{1, 1, 1};
  double round = 0.6;
  double blend = 0.8, rough = 0.0, grain = 4.0;
  bool carve = false;
  uint32_t c1 = 0x808080, c2 = 0x808080;
  double mottle = 0, shine = 0, glow = 0;
  Vec bc{0, 0, 0};             // a sphere round it all, for skipping it far away
  double br = 0;
  int seed = 0;
  // its distance (negative inside), without its roughness; f: where along its gradient the point is (0..1)
  double dist(Vec p, double* f) const {
    if (kind == 0) {
      // a round cone (Inigo Quilez's): the capsule of two spheres of radii r0, r1 and the cone that joins them
      const Vec ba = vsub(b, a), pa = vsub(p, a);
      const double l2 = vdot(ba, ba);
      const double rr = r0 - r1, a2 = l2 - rr * rr, il2 = 1.0 / std::max(l2, 1e-9);
      const double y = vdot(pa, ba), z = y - l2;
      const Vec xv = vsub(vmul(pa, l2), vmul(ba, y));
      const double x2 = vdot(xv, xv), y2 = y * y * l2, z2 = z * z * l2;
      const double k = (rr > 0 ? 1.0 : -1.0) * rr * rr * x2;
      *f = std::min(1.0, std::max(0.0, y * il2));
      if ((z > 0 ? 1.0 : -1.0) * a2 * z2 > k) return std::sqrt(x2 + z2) * il2 - r1;
      if ((y > 0 ? 1.0 : -1.0) * a2 * y2 < k) return std::sqrt(x2 + y2) * il2 - r0;
      return (std::sqrt(x2 * a2 * il2) + y * rr) * il2 - r0;
    }
    const Vec d = vsub(p, a);
    const Vec q{R[0] * d.x + R[1] * d.y + R[2] * d.z, R[3] * d.x + R[4] * d.y + R[5] * d.z, R[6] * d.x + R[7] * d.y + R[8] * d.z};
    if (kind == 1) {
      *f = std::min(1.0, std::max(0.0, (q.z / half.z + 1) * 0.5));
      const Vec q0{q.x / half.x, q.y / half.y, q.z / half.z}, q1{q.x / (half.x * half.x), q.y / (half.y * half.y), q.z / (half.z * half.z)};
      const double k0 = vlen(q0), k1 = vlen(q1);
      return k1 > 1e-12 ? k0 * (k0 - 1.0) / k1 : -std::min({half.x, half.y, half.z});
    }
    *f = std::min(1.0, std::max(0.0, (q.z / half.z + 1) * 0.5));
    const double rd = std::min({round, half.x, half.y, half.z});
    const Vec e{std::fabs(q.x) - half.x + rd, std::fabs(q.y) - half.y + rd, std::fabs(q.z) - half.z + rd};
    const Vec m{std::max(e.x, 0.0), std::max(e.y, 0.0), std::max(e.z, 0.0)};
    return vlen(m) + std::min(std::max(e.x, std::max(e.y, e.z)), 0.0) - rd;
  }
};

// the polynomial smooth minimum: a and b joined with a fillet k wide
inline double smin(double a, double b, double k) {
  if (k <= 0) return std::min(a, b);
  const double h = std::max(k - std::fabs(a - b), 0.0) / k;
  return std::min(a, b) - h * h * k * 0.25;
}
inline double smax(double a, double b, double k) { return -smin(-a, -b, k); }

struct Sculpt {
  std::vector<Shape> shapes;
  // the surface's distance at p (negative inside)
  double field(Vec p) const {
    double d = 1e9;
    for (const Shape& s : shapes) {
      if (s.carve) continue;
      const double far = vlen(vsub(p, s.bc)) - s.br;
      if (far > d + s.blend + s.rough) continue;
      double f;
      double di = s.dist(p, &f);
      if (s.rough > 0) di -= s.rough * (fbm(p.x / s.grain + s.seed * 13.7, p.y / s.grain, p.z / s.grain) - 0.5) * 2.0;
      d = smin(d, di, s.blend);
    }
    for (const Shape& s : shapes) {
      if (!s.carve) continue;
      if (vlen(vsub(p, s.bc)) - s.br > s.blend) continue;
      double f;
      d = smax(d, -s.dist(p, &f), s.blend);
    }
    return d;
  }
};
}  // namespace

Value bi_r3d_mesh(Vm&, std::vector<Value>& args, std::string& error) {
  auto t = get_target(args[0], error, "r3d_mesh");
  if (!t) return Value::nil();
  if (!want_number(args[1], error, "r3d_mesh") || !want_list(args[2], error, "r3d_mesh")) return Value::nil();
  if (args[3].type != Type::Map) { error = "r3d_mesh(): the fourth argument wants a map {rest, cell}"; return Value::nil(); }
  auto* opt = static_cast<MapObj*>(args[3].obj.get());
  double cell = 1.0;
  field(opt, "cell", cell);
  // as fine as a weapon held at the eye wants (a pistol is seven units long); the grid itself is bounded below
  if (!(cell >= 0.05 && cell <= 8)) { error = "r3d_mesh(): cell wants 0.05..8 units"; return Value::nil(); }
  Value* restv = opt->find("rest");
  if (!restv || restv->type != Type::List || restv->list_items().empty() || restv->list_items().size() % 12 != 0) {
    error = "r3d_mesh(): rest wants the bones' rest matrices, 12 numbers a bone";
    return Value::nil();
  }
  const auto t0 = std::chrono::steady_clock::now();
  const int nb = (int)(restv->list_items().size() / 12);
  if (nb > 64) { error = "r3d_mesh(): at most 64 bones"; return Value::nil(); }
  std::vector<double> rest((size_t)nb * 12);
  for (size_t i = 0; i < rest.size(); ++i) {
    const Value& e = restv->list_items()[i];
    if (!e.is_number()) { error = "r3d_mesh(): rest wants numbers"; return Value::nil(); }
    rest[i] = e.as_double();
  }
  Sculpt sc;
  size_t pi = 0;
  for (const Value& pv : args[2].list_items()) {
    if (pv.type != Type::Map) { error = "r3d_mesh(): part " + std::to_string(pi) + " is not a map"; return Value::nil(); }
    auto* m = static_cast<MapObj*>(pv.obj.get());
    Value* kv = m->find("kind");
    const std::string kind = kv && kv->type == Type::Str ? kv->str() : "";
    Shape s;
    double bone = 0, col = 0x808080, col2 = -1;
    field(m, "bone", bone); field(m, "col", col); field(m, "col2", col2);
    field(m, "mottle", s.mottle); field(m, "grain", s.grain); field(m, "shine", s.shine); field(m, "glow", s.glow);
    field(m, "blend", s.blend); field(m, "rough", s.rough); field(m, "round", s.round);
    if (Value* cv = m->find("carve")) s.carve = cv->type == Type::Bool && cv->b;
    if (bone < 0 || bone >= nb) { error = "r3d_mesh(): part " + std::to_string(pi) + "'s bone " + std::to_string((int)bone) + " has no rest matrix"; return Value::nil(); }
    s.bone = (int)bone;
    s.c1 = (uint32_t)col;
    s.c2 = col2 < 0 ? s.c1 : (uint32_t)col2;
    s.grain = std::max(0.5, s.grain);
    s.seed = (int)pi;
    const double* M = &rest[(size_t)s.bone * 12];
    if (kind == "limb") {
      double a[3], b[3];
      if (!vec_field(m, "a", a, 3, error, pi) || !vec_field(m, "b", b, 3, error, pi) || !field(m, "r0", s.r0)) {
        if (error.empty()) error = "r3d_mesh(): limb part " + std::to_string(pi) + " needs a, b and r0";
        return Value::nil();
      }
      if (!field(m, "r1", s.r1)) s.r1 = s.r0;
      s.kind = 0;
      s.a = m34_point(M, {a[0], a[1], a[2]});
      s.b = m34_point(M, {b[0], b[1], b[2]});
      if (vlen(vsub(s.b, s.a)) < 1e-6) s.b = vadd(s.a, {0, 0, 1e-3});
      s.bc = vmul(vadd(s.a, s.b), 0.5);
      s.br = vlen(vsub(s.b, s.a)) * 0.5 + std::max(s.r0, s.r1);
    } else if (kind == "ellipsoid" || kind == "box") {
      double c[3], r[3], ypr[3] = {0, 0, 0};
      if (!vec_field(m, "c", c, 3, error, pi) || !vec_field(m, "r", r, 3, error, pi)) {
        if (error.empty()) error = "r3d_mesh(): " + kind + " part " + std::to_string(pi) + " needs c and r";
        return Value::nil();
      }
      if (m->find("rot") && !vec_field(m, "rot", ypr, 3, error, pi)) return Value::nil();
      s.kind = kind == "ellipsoid" ? 1 : 2;
      s.a = m34_point(M, {c[0], c[1], c[2]});
      s.half = {std::max(0.05, r[0]), std::max(0.05, r[1]), std::max(0.05, r[2])};
      // the shape's axes in the model's space: the bone's rotation, then its own; R takes model to shape
      double Rs[9];
      rot_rows(ypr, Rs);
      double W[9];
      for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) W[i * 3 + j] = M[i * 4] * Rs[j] + M[i * 4 + 1] * Rs[3 + j] + M[i * 4 + 2] * Rs[6 + j];
      for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) s.R[i * 3 + j] = W[j * 3 + i];
      s.bc = s.a;
      s.br = vlen(s.half);
    } else {
      error = "r3d_mesh(): part " + std::to_string(pi) + "'s kind is \"" + kind + "\": limb, ellipsoid or box";
      return Value::nil();
    }
    s.br += s.rough;
    sc.shapes.push_back(s);
    ++pi;
  }
  if (sc.shapes.empty()) { error = "r3d_mesh(): no parts"; return Value::nil(); }
  // the grid round it all
  Vec lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
  for (const Shape& s : sc.shapes) {
    if (s.carve) continue;
    const double r = s.br + s.blend + cell * 2;
    lo = {std::min(lo.x, s.bc.x - r), std::min(lo.y, s.bc.y - r), std::min(lo.z, s.bc.z - r)};
    hi = {std::max(hi.x, s.bc.x + r), std::max(hi.y, s.bc.y + r), std::max(hi.z, s.bc.z + r)};
  }
  const int NX = (int)std::ceil((hi.x - lo.x) / cell) + 1, NY = (int)std::ceil((hi.y - lo.y) / cell) + 1, NZ = (int)std::ceil((hi.z - lo.z) / cell) + 1;
  if ((int64_t)NX * NY * NZ > 24 * 1024 * 1024) { error = "r3d_mesh(): too fine a cell for the model's size"; return Value::nil(); }
  std::vector<float> F((size_t)NX * NY * NZ);
  auto gi = [&](int i, int j, int k) { return ((size_t)k * NY + j) * NX + i; };
  SqlThreadPool::instance().for_chunks((size_t)NZ, 1, (size_t)NX * NY * NZ * 8, false, [&](size_t k0, size_t k1, size_t) {
    for (size_t k = k0; k < k1; ++k)
      for (int j = 0; j < NY; ++j)
        for (int i = 0; i < NX; ++i)
          F[gi(i, j, (int)k)] = (float)sc.field({lo.x + i * cell, lo.y + j * cell, lo.z + k * cell});
  });
  // Surface nets: a vertex in each cell the surface crosses, at the mean of where it crosses the cell's edges;
  // a quad across each grid edge it crosses, joining the four cells round that edge.
  std::vector<int32_t> cellv((size_t)NX * NY * NZ, -1);
  std::vector<Vec> pos;
  static const int corner[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
  static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  for (int k = 0; k + 1 < NZ; ++k)
    for (int j = 0; j + 1 < NY; ++j)
      for (int i = 0; i + 1 < NX; ++i) {
        float v[8];
        int inside = 0;
        for (int c = 0; c < 8; ++c) {
          v[c] = F[gi(i + corner[c][0], j + corner[c][1], k + corner[c][2])];
          inside += v[c] < 0;
        }
        if (inside == 0 || inside == 8) continue;
        Vec acc{0, 0, 0};
        int n = 0;
        for (const auto& e : edges) {
          const float a = v[e[0]], b = v[e[1]];
          if ((a < 0) == (b < 0)) continue;
          const double tt = a / (double)(a - b);
          const int* ca = corner[e[0]], *cb = corner[e[1]];
          acc = vadd(acc, {ca[0] + (cb[0] - ca[0]) * tt, ca[1] + (cb[1] - ca[1]) * tt, ca[2] + (cb[2] - ca[2]) * tt});
          ++n;
        }
        acc = vmul(acc, 1.0 / n);
        cellv[gi(i, j, k)] = (int32_t)pos.size();
        pos.push_back({lo.x + (i + acc.x) * cell, lo.y + (j + acc.y) * cell, lo.z + (k + acc.z) * cell});
      }
  auto mesh = std::make_shared<R3dMesh>();
  for (int k = 1; k + 1 < NZ; ++k)
    for (int j = 1; j + 1 < NY; ++j)
      for (int i = 1; i + 1 < NX; ++i) {
        const float f0 = F[gi(i, j, k)];
        for (int ax = 0; ax < 3; ++ax) {
          const int di = ax == 0, dj = ax == 1, dk = ax == 2;
          if (i + di >= NX || j + dj >= NY || k + dk >= NZ) continue;
          const float f1 = F[gi(i + di, j + dj, k + dk)];
          if ((f0 < 0) == (f1 < 0)) continue;
          // the four cells sharing this edge
          int c[4];
          if (ax == 0) { c[0] = cellv[gi(i, j - 1, k - 1)]; c[1] = cellv[gi(i, j, k - 1)]; c[2] = cellv[gi(i, j, k)]; c[3] = cellv[gi(i, j - 1, k)]; }
          else if (ax == 1) { c[0] = cellv[gi(i - 1, j, k - 1)]; c[1] = cellv[gi(i - 1, j, k)]; c[2] = cellv[gi(i, j, k)]; c[3] = cellv[gi(i, j, k - 1)]; }
          else { c[0] = cellv[gi(i - 1, j - 1, k)]; c[1] = cellv[gi(i, j - 1, k)]; c[2] = cellv[gi(i, j, k)]; c[3] = cellv[gi(i - 1, j, k)]; }
          if (c[0] < 0 || c[1] < 0 || c[2] < 0 || c[3] < 0) continue;
          const bool flip = f0 < 0;
          if (flip) { mesh->idx.insert(mesh->idx.end(), {(uint32_t)c[0], (uint32_t)c[1], (uint32_t)c[2], (uint32_t)c[0], (uint32_t)c[2], (uint32_t)c[3]}); }
          else { mesh->idx.insert(mesh->idx.end(), {(uint32_t)c[0], (uint32_t)c[2], (uint32_t)c[1], (uint32_t)c[0], (uint32_t)c[3], (uint32_t)c[2]}); }
        }
      }
  // each vertex: onto the surface (a step along the field's gradient), its normal the gradient, its colour, shine
  // and glow from the shapes nearest it (blended across their seams), its bones the two nearest
  mesh->v.resize(pos.size());
  SqlThreadPool::instance().for_chunks(pos.size(), 256, pos.size() * 64, false, [&](size_t v0, size_t v1, size_t) {
    std::vector<double> wsum((size_t)nb);
    for (size_t vi = v0; vi < v1; ++vi) {
      Vec p = pos[vi];
      const double h = cell * 0.25;
      auto grad = [&](Vec q) {
        return Vec{sc.field({q.x + h, q.y, q.z}) - sc.field({q.x - h, q.y, q.z}), sc.field({q.x, q.y + h, q.z}) - sc.field({q.x, q.y - h, q.z}),
                   sc.field({q.x, q.y, q.z + h}) - sc.field({q.x, q.y, q.z - h})};
      };
      for (int it = 0; it < 2; ++it) {
        const double d = sc.field(p);
        const Vec g = grad(p);
        const double g2 = vdot(g, g) / (4 * h * h);
        if (g2 > 1e-9) p = vsub(p, vmul(g, d / (2 * h) / g2));
      }
      const Vec n = vnorm(grad(p));
      double R = 0, G = 0, B = 0, S = 0, Gl = 0, W = 0;
      std::fill(wsum.begin(), wsum.end(), 0.0);
      double dmin = 1e9;
      std::vector<std::pair<double, size_t>> near;
      for (size_t si = 0; si < sc.shapes.size(); ++si) {
        const Shape& s = sc.shapes[si];
        if (s.carve) continue;
        if (vlen(vsub(p, s.bc)) - s.br > dmin + 6.0) continue;
        double f;
        const double d = s.dist(p, &f);
        dmin = std::min(dmin, d);
        near.push_back({d, si});
      }
      // the shape the point lies on: a seam's colour spreads as far as the seam's own fillet, the smaller of the two
      // shapes' blends - wide between two soft forms, sharp where a hand meets steel (blend 0.35 and over spread as
      // they always did)
      double blend_here = 0.8;
      for (const auto& nd : near)
        if (nd.first == dmin) { blend_here = sc.shapes[nd.second].blend; break; }
      for (const auto& nd : near) {
        const Shape& s = sc.shapes[nd.second];
        double f;
        s.dist(p, &f);
        const double bseam = std::min(s.blend, blend_here);
        // the colour's spread: 0.35 units for a seam of blend 0.35 and over (as always), falling off faster than the
        // blend below it so a hard seam (a cuff on skin, a hand on steel) changes colour at its edge, not in a fade
        const double cfall = bseam >= 0.35 ? std::max(0.35, bseam * 0.35) : std::max(0.01, 0.35 * std::pow(bseam / 0.35, 1.5));
        const double w = std::exp(-(nd.first - dmin) / cfall);
        double r = ((s.c1 >> 16) & 255) * (1 - f) + ((s.c2 >> 16) & 255) * f;
        double g = ((s.c1 >> 8) & 255) * (1 - f) + ((s.c2 >> 8) & 255) * f;
        double b = (s.c1 & 255) * (1 - f) + (s.c2 & 255) * f;
        if (s.mottle > 0) {
          const double gs = 1.0 / s.grain;
          const double k = 1.0 + s.mottle * (fbm(p.x * gs + 3.1 * s.seed, p.y * gs, p.z * gs) - 0.5) * 1.8;
          r *= k; g *= k; b *= k;
        }
        R += w * r; G += w * g; B += w * b; S += w * s.shine; Gl += w * s.glow; W += w;
        // its bones likewise: flesh bends across a soft seam (1.2 units, blend 0.35 and over), a hard one is rigid -
        // a slide, a pump, a muzzle flash moves alone, not dragging the frame, barrel or bore beside it into a sheet
        const double hard = std::min(1.0, std::max(0.0, (bseam - 0.02) / 0.33));
        const double bone_fall = std::max(0.01, 1.2 * hard * hard);
        wsum[(size_t)s.bone] += std::exp(-(nd.first - dmin) / bone_fall);
      }
      int b0 = 0, b1 = 0;
      for (int b = 0; b < nb; ++b) if (wsum[(size_t)b] > wsum[(size_t)b0]) b0 = b;
      b1 = b0;
      for (int b = 0; b < nb; ++b) if (b != b0 && (b1 == b0 || wsum[(size_t)b] > wsum[(size_t)b1])) b1 = b;
      const double w0 = wsum[(size_t)b0] + wsum[(size_t)b1] > 0 ? wsum[(size_t)b0] / (wsum[(size_t)b0] + wsum[(size_t)b1]) : 1.0;
      W = std::max(W, 1e-9);
      // a cavity (where the surface folds in) is darker: the field a little way out along the normal tells
      const double occ = std::min(1.0, std::max(0.35, sc.field(vadd(p, vmul(n, 2.5))) / 2.5));
      R3dMesh::V V{(float)p.x, (float)p.y, (float)p.z, (float)n.x, (float)n.y, (float)n.z,
                   (float)(R / W / 255.0 * occ), (float)(G / W / 255.0 * occ), (float)(B / W / 255.0 * occ),
                   (float)(S / W), (float)(Gl / W), {(uint8_t)b0, (uint8_t)b1}, (float)w0};
      mesh->v[vi] = V;
    }
  });
  mesh->bones = nb;
  mesh->inv_rest.resize((size_t)nb * 12);
  for (int b = 0; b < nb; ++b) m34_inverse_rigid(&rest[(size_t)b * 12], &mesh->inv_rest[(size_t)b * 12]);
  for (const auto& V : mesh->v) {
    mesh->radius = std::max(mesh->radius, std::hypot((double)V.x, (double)V.y));
    mesh->height = std::max(mesh->height, (double)V.z);
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  auto out = std::make_shared<MapObj>();
  out->put("verts", Value::integer((int64_t)mesh->v.size()));
  out->put("tris", Value::integer((int64_t)(mesh->idx.size() / 3)));
  out->put("ms", Value::real(ms));
  t->meshes[args[1].as_int()] = std::move(mesh);
  return Value::object(Type::Map, out);
}

// ---- models (a skeleton, skinned textured parts, animations: glTF's shape) ---------------------------------------
//
//   r3d_texture_rgba(t, id, w, h, px[, layout]) -> id: a true-colour picture, top row first, with mipmaps; px bytes
//       (or a string of them), 4 a pixel: R G B A, or with layout "argb" a little-endian 0xAARRGGBB word each (what
//       canvas_to_bytes(canvas, "argb") gives: an image_decode'd picture as it is)
//   r3d_model(t, id, spec)                -> {nodes, joints, tris}
//       spec: {nodes: [{parent, t: [x y z], r: [x y z w], s: [x y z]}]  (each node's rest transform in its parent's)
//              joints: [node ids], inv_bind: bytes (a 4x4 float32 matrix a joint, column after column),
//              prims: [{node, skinned, tex (an r3d_texture_rgba id, or -1), color: [r g b a],
//                       positions, normals, uvs, joints, weights (bytes: float32 xyz, xyz, uv; uint32 x4; float32 x4),
//                       indices (bytes, uint32 each), colors (float32 rgb a vertex), mats (float32 shine glow)}],
//              up: "y" (glTF's: y up, the model facing +z) or "z" (x forward, y left, z up, as everything here),
//              scale: units a model unit}
//   r3d_anim(t, model id, anim id, channels) -> its duration (seconds)
//       channels: [{node, path ("translation" | "rotation" | "scale"), interp ("LINEAR" | "STEP" | "CUBICSPLINE"),
//                   times (float32 each), vals (float32: 3 or 4 a key; three times that for CUBICSPLINE)}]
//   r3d_draw_tex's things also take {model, anim, time, hold, anim2, time2, hold2, mix, scale, ...}: the model posed
//   by its animation at that time (seconds: round its length again and again, or with hold once and then held at
//   its end), blended mix of the way to anim2 at time2.

struct R3dModel {
  struct Node { int parent = -1; double t[3] = {0, 0, 0}, r[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1}; };
  struct Vert { float p[3], n[3], uv[2]; uint8_t j[4]; float w[4]; float c[3] = {1, 1, 1}; float shine = 0, glow = 0; };
  struct Prim { int node = 0; bool skinned = false; std::shared_ptr<R3dTex> tex; float col[4] = {1, 1, 1, 1}; bool relief = false; std::vector<Vert> v; std::vector<uint32_t> idx; };
  struct Channel { int node = 0; int path = 0; int interp = 0; std::vector<float> times, vals; };   // path 0 t, 1 r, 2 s; interp 0 linear, 1 step, 2 cubic
  struct Anim { std::vector<Channel> ch; double duration = 0; };
  std::vector<Node> nodes;
  std::vector<int> order;            // nodes parents first
  std::vector<int> joints;
  std::vector<double> inv_bind;      // 12 a joint, row-major 3x4
  std::vector<Prim> prims;
  std::unordered_map<int64_t, Anim> anims;
  double root[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};   // the model's space into ours (axes and scale)
  double radius = 0, height = 0;
};

namespace {
// a rotation, translation and scale as a row-major 3x4
void trs_matrix(const double* t, const double* q, const double* s, double* M) {
  const double x = q[0], y = q[1], z = q[2], w = q[3];
  const double xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
  M[0] = (1 - 2 * (yy + zz)) * s[0]; M[1] = 2 * (xy - wz) * s[1]; M[2] = 2 * (xz + wy) * s[2]; M[3] = t[0];
  M[4] = 2 * (xy + wz) * s[0]; M[5] = (1 - 2 * (xx + zz)) * s[1]; M[6] = 2 * (yz - wx) * s[2]; M[7] = t[1];
  M[8] = 2 * (xz - wy) * s[0]; M[9] = 2 * (yz + wx) * s[1]; M[10] = (1 - 2 * (xx + yy)) * s[2]; M[11] = t[2];
}
void nlerp_q(const double* a, const double* b, double k, double* o) {
  const double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  const double sgn = d < 0 ? -1.0 : 1.0;
  double l = 0;
  for (int i = 0; i < 4; ++i) { o[i] = a[i] * (1 - k) + b[i] * sgn * k; l += o[i] * o[i]; }
  l = l > 1e-12 ? 1.0 / std::sqrt(l) : 0;
  for (int i = 0; i < 4; ++i) o[i] *= l;
}
// one channel's value at time tm (n numbers: 3 or 4)
void sample_channel(const R3dModel::Channel& c, double tm, double* out) {
  const int n = c.path == 1 ? 4 : 3;
  const size_t keys = c.times.size();
  const int per = c.interp == 2 ? 3 : 1;   // a cubic key: in-tangent, value, out-tangent
  auto val = [&](size_t k, int part, int i) { return (double)c.vals[(k * per + (per == 3 ? part : 0)) * n + i]; };
  if (keys == 0) return;
  if (tm <= c.times[0] || keys == 1) { for (int i = 0; i < n; ++i) out[i] = val(0, 1, i); return; }
  if (tm >= c.times[keys - 1]) { for (int i = 0; i < n; ++i) out[i] = val(keys - 1, 1, i); return; }
  size_t lo = 0, hi = keys - 1;
  while (hi - lo > 1) { const size_t mid = (lo + hi) / 2; if (c.times[mid] <= tm) lo = mid; else hi = mid; }
  const double t0 = c.times[lo], t1 = c.times[hi], dt = std::max(1e-9, t1 - t0), k = (tm - t0) / dt;
  if (c.interp == 1) { for (int i = 0; i < n; ++i) out[i] = val(lo, 1, i); return; }
  if (c.interp == 2) {
    const double k2 = k * k, k3 = k2 * k;
    const double h00 = 2 * k3 - 3 * k2 + 1, h10 = k3 - 2 * k2 + k, h01 = -2 * k3 + 3 * k2, h11 = k3 - k2;
    for (int i = 0; i < n; ++i) out[i] = h00 * val(lo, 1, i) + h10 * dt * val(lo, 2, i) + h01 * val(hi, 1, i) + h11 * dt * val(hi, 0, i);
    if (n == 4) { double l = 0; for (int i = 0; i < 4; ++i) l += out[i] * out[i]; l = l > 1e-12 ? 1 / std::sqrt(l) : 0; for (int i = 0; i < 4; ++i) out[i] *= l; }
    return;
  }
  if (n == 4) { const double a[4] = {val(lo, 1, 0), val(lo, 1, 1), val(lo, 1, 2), val(lo, 1, 3)}, b[4] = {val(hi, 1, 0), val(hi, 1, 1), val(hi, 1, 2), val(hi, 1, 3)}; nlerp_q(a, b, k, out); return; }
  for (int i = 0; i < n; ++i) out[i] = val(lo, 1, i) * (1 - k) + val(hi, 1, i) * k;
}
// every node's local transform posed: rest, overwritten by what the animation moves
void pose_nodes(const R3dModel& m, const R3dModel::Anim* a, double tm, bool hold, std::vector<R3dModel::Node>* out) {
  *out = m.nodes;
  if (!a) return;
  if (hold) tm = std::min(std::max(tm, 0.0), a->duration);   // played once, then held at its end
  else if (a->duration > 0) tm = std::fmod(std::fmod(tm, a->duration) + a->duration, a->duration);
  for (const auto& c : a->ch) {
    if (c.node < 0 || (size_t)c.node >= out->size()) continue;
    auto& nd = (*out)[(size_t)c.node];
    sample_channel(c, tm, c.path == 0 ? nd.t : c.path == 1 ? nd.r : nd.s);
  }
}
}  // namespace

// r3d_mesh_data(t, id) -> a sculpted mesh (r3d_mesh's) as packed data, the way models are kept (r3d_model's prims):
//   {positions, normals (float32 xyz), colors (float32 rgb), mats (float32 shine glow), joints (uint32 x4),
//    weights (float32 x4), indices (uint32), verts, tris} - its points in the model's space at rest
Value bi_r3d_mesh_data(Vm&, std::vector<Value>& args, std::string& error) {
  auto t = get_target(args[0], error, "r3d_mesh_data");
  if (!t) return Value::nil();
  if (!want_number(args[1], error, "r3d_mesh_data")) return Value::nil();
  auto it = t->meshes.find(args[1].as_int());
  if (it == t->meshes.end()) { error = "r3d_mesh_data(): no mesh " + std::to_string(args[1].as_int()); return Value::nil(); }
  const R3dMesh& m = *it->second;
  const size_t n = m.v.size();
  std::vector<uint8_t> P(n * 12), N(n * 12), C(n * 12), M(n * 8), J(n * 16, 0), W(n * 16, 0), I(m.idx.size() * 4);
  auto putf = [](std::vector<uint8_t>& b, size_t i, float f) { std::memcpy(&b[i * 4], &f, 4); };
  auto putu = [](std::vector<uint8_t>& b, size_t i, uint32_t u) { std::memcpy(&b[i * 4], &u, 4); };
  for (size_t i = 0; i < n; ++i) {
    const auto& V = m.v[i];
    putf(P, i * 3, V.x); putf(P, i * 3 + 1, V.y); putf(P, i * 3 + 2, V.z);
    putf(N, i * 3, V.nx); putf(N, i * 3 + 1, V.ny); putf(N, i * 3 + 2, V.nz);
    putf(C, i * 3, V.r); putf(C, i * 3 + 1, V.g); putf(C, i * 3 + 2, V.b);
    putf(M, i * 2, V.shine); putf(M, i * 2 + 1, V.glow);
    putu(J, i * 4, V.bone[0]); putu(J, i * 4 + 1, V.bone[1]);
    putf(W, i * 4, V.w0); putf(W, i * 4 + 1, 1.0f - V.w0);
  }
  for (size_t i = 0; i < m.idx.size(); ++i) putu(I, i, m.idx[i]);
  auto out = std::make_shared<MapObj>();
  out->put("positions", Value::bytes(std::move(P)));
  out->put("normals", Value::bytes(std::move(N)));
  out->put("colors", Value::bytes(std::move(C)));
  out->put("mats", Value::bytes(std::move(M)));
  out->put("joints", Value::bytes(std::move(J)));
  out->put("weights", Value::bytes(std::move(W)));
  out->put("indices", Value::bytes(std::move(I)));
  out->put("verts", Value::integer((int64_t)n));
  out->put("tris", Value::integer((int64_t)(m.idx.size() / 3)));
  return Value::object(Type::Map, out);
}

Value bi_r3d_texture_rgba(Vm&, std::vector<Value>& args, std::string& error) {
  auto t = get_target(args[0], error, "r3d_texture_rgba");
  if (!t) return Value::nil();
  for (int i = 1; i <= 3; ++i) if (!want_number(args[(size_t)i], error, "r3d_texture_rgba")) return Value::nil();
  const int64_t id = args[1].as_int(), w = args[2].as_int(), h = args[3].as_int();
  if (id < 0 || id > 1000000) { error = "r3d_texture_rgba(): id out of range (0..1000000)"; return Value::nil(); }
  if (w < 1 || h < 1 || w > 8192 || h > 8192) { error = "r3d_texture_rgba(): size out of range (1..8192)"; return Value::nil(); }
  const uint8_t* px = nullptr;
  size_t plen = 0;
  if (args[4].type == Type::Bytes) { px = args[4].bytes_data().data(); plen = args[4].bytes_data().size(); }
  else if (args[4].type == Type::Str) { px = (const uint8_t*)args[4].str().data(); plen = args[4].str().size(); }
  if (!px || plen < (size_t)(w * h * 4)) { error = "r3d_texture_rgba(): the pixels want w x h x 4 bytes"; return Value::nil(); }
  const bool argb = args.size() > 5 && args[5].type == Type::Str && args[5].str() == "argb";
  if (args.size() > 5 && args[5].type != Type::Nil && !argb) { error = "r3d_texture_rgba(): layout wants \"argb\" (or nothing, for R G B A)"; return Value::nil(); }
  auto tx = std::make_shared<R3dTex>();
  R3dTex::Level L0;
  L0.w = (int)w; L0.h = (int)h;
  L0.px.resize((size_t)w * h);
  for (size_t i = 0; i < L0.px.size(); ++i) {
    const uint8_t* q = px + i * 4;
    const uint32_t v = argb ? ((uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24))
                            : (((uint32_t)q[3] << 24) | ((uint32_t)q[0] << 16) | ((uint32_t)q[1] << 8) | q[2]);
    if ((v >> 24) < 255) tx->alpha = true;
    L0.px[i] = v;
  }
  tx->mips.push_back(std::move(L0));
  while (tx->mips.back().w > 1 || tx->mips.back().h > 1) tx->mips.push_back(half_of(tx->mips.back()));
  if ((size_t)id >= t->tex.size()) t->tex.resize((size_t)id + 1);
  t->tex[(size_t)id] = std::move(tx);
  return Value::integer(id);
}

namespace {
bool num_list(const Value* v, double* out, int n) {
  if (!v || v->type != Type::List || v->list_items().size() != (size_t)n) return false;
  for (int i = 0; i < n; ++i) { if (!v->list_items()[(size_t)i].is_number()) return false; out[i] = v->list_items()[(size_t)i].as_double(); }
  return true;
}
// packed binary data a map member holds: bytes, or a string of them (what sql() gives for a BLOB column)
struct Blob {
  const uint8_t* p = nullptr;
  size_t n = 0;
  size_t size() const { return n; }
};
std::optional<Blob> bytes_member(MapObj* m, const char* k) {
  Value* v = m->find(k);
  if (!v) return std::nullopt;
  if (v->type == Type::Bytes) return Blob{v->bytes_data().data(), v->bytes_data().size()};
  if (v->type == Type::Str) return Blob{(const uint8_t*)v->str().data(), v->str().size()};
  return std::nullopt;
}
float f32_at(const Blob& b, size_t i) { float f; std::memcpy(&f, b.p + i * 4, 4); return f; }
uint32_t u32_at(const Blob& b, size_t i) { uint32_t u; std::memcpy(&u, b.p + i * 4, 4); return u; }
}  // namespace

Value bi_r3d_model(Vm&, std::vector<Value>& args, std::string& error) {
  auto t = get_target(args[0], error, "r3d_model");
  if (!t) return Value::nil();
  if (!want_number(args[1], error, "r3d_model")) return Value::nil();
  if (args[2].type != Type::Map) { error = "r3d_model(): the spec wants a map {nodes, joints, inv_bind, prims, up, scale}"; return Value::nil(); }
  auto* spec = static_cast<MapObj*>(args[2].obj.get());
  auto md = std::make_shared<R3dModel>();
  Value* nodes = spec->find("nodes");
  if (!nodes || nodes->type != Type::List || nodes->list_items().empty()) { error = "r3d_model(): nodes wants a list of {parent, t, r, s}"; return Value::nil(); }
  for (const Value& nv : nodes->list_items()) {
    if (nv.type != Type::Map) { error = "r3d_model(): each node is a map"; return Value::nil(); }
    auto* m = static_cast<MapObj*>(nv.obj.get());
    R3dModel::Node nd;
    double par = -1;
    field(m, "parent", par);
    nd.parent = (int)par;
    if (m->find("t") && !num_list(m->find("t"), nd.t, 3)) { error = "r3d_model(): a node's t wants 3 numbers"; return Value::nil(); }
    if (m->find("r") && !num_list(m->find("r"), nd.r, 4)) { error = "r3d_model(): a node's r wants 4 numbers (a quaternion x y z w)"; return Value::nil(); }
    if (m->find("s") && !num_list(m->find("s"), nd.s, 3)) { error = "r3d_model(): a node's s wants 3 numbers"; return Value::nil(); }
    md->nodes.push_back(nd);
  }
  const int nn = (int)md->nodes.size();
  for (const auto& nd : md->nodes) if (nd.parent < -1 || nd.parent >= nn) { error = "r3d_model(): a node's parent is out of range"; return Value::nil(); }
  // parents first (and no cycles)
  {
    std::vector<int> state((size_t)nn, 0);
    std::function<bool(int)> visit = [&](int i) {
      if (state[(size_t)i] == 2) return true;
      if (state[(size_t)i] == 1) return false;
      state[(size_t)i] = 1;
      if (md->nodes[(size_t)i].parent >= 0 && !visit(md->nodes[(size_t)i].parent)) return false;
      state[(size_t)i] = 2;
      md->order.push_back(i);
      return true;
    };
    for (int i = 0; i < nn; ++i) if (!visit(i)) { error = "r3d_model(): the nodes' parents form a cycle"; return Value::nil(); }
  }
  if (Value* jv = spec->find("joints"); jv && jv->type == Type::List) {
    for (const Value& j : jv->list_items()) {
      if (!j.is_number() || j.as_int() < 0 || j.as_int() >= nn) { error = "r3d_model(): joints wants node ids"; return Value::nil(); }
      md->joints.push_back((int)j.as_int());
    }
    const auto ib = bytes_member(spec, "inv_bind");
    if (!md->joints.empty() && (!ib || ib->size() < md->joints.size() * 64)) { error = "r3d_model(): inv_bind wants a 4x4 float32 matrix a joint"; return Value::nil(); }
    if (md->joints.size() > 255) { error = "r3d_model(): at most 255 joints"; return Value::nil(); }
    md->inv_bind.resize(md->joints.size() * 12);
    for (size_t j = 0; j < md->joints.size(); ++j)
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) md->inv_bind[j * 12 + (size_t)r * 4 + c] = f32_at(*ib, j * 16 + (size_t)c * 4 + r);   // column-major in
  }
  double scale = 1;
  field(spec, "scale", scale);
  Value* up = spec->find("up");
  const bool yup = !up || (up->type == Type::Str && up->str() == "y");
  if (yup) {   // glTF's y up, facing +z -> ours: x forward (their z), y left (their x), z up (their y)
    const double R[12] = {0, 0, scale, 0, scale, 0, 0, 0, 0, scale, 0, 0};
    std::memcpy(md->root, R, sizeof R);
  } else {
    const double R[12] = {scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, scale, 0};
    std::memcpy(md->root, R, sizeof R);
  }
  Value* prims = spec->find("prims");
  if (!prims || prims->type != Type::List) { error = "r3d_model(): prims wants a list"; return Value::nil(); }
  int64_t tris = 0;
  size_t pi = 0;
  for (const Value& pv : prims->list_items()) {
    if (pv.type != Type::Map) { error = "r3d_model(): prim " + std::to_string(pi) + " is not a map"; return Value::nil(); }
    auto* m = static_cast<MapObj*>(pv.obj.get());
    R3dModel::Prim pr;
    double nodev = 0, texv = -1;
    field(m, "node", nodev); field(m, "tex", texv);
    pr.node = (int)nodev;
    if (pr.node < 0 || pr.node >= nn) { error = "r3d_model(): prim " + std::to_string(pi) + "'s node is out of range"; return Value::nil(); }
    if (Value* sk = m->find("skinned")) pr.skinned = sk->type == Type::Bool ? sk->b : (sk->is_number() && sk->as_int() != 0);
    if (texv >= 0) { pr.tex = tex_of(*t, (int64_t)texv); if (!pr.tex) { error = "r3d_model(): prim " + std::to_string(pi) + " names texture " + std::to_string((int64_t)texv) + ", which was not given"; return Value::nil(); } }
    double col[4] = {1, 1, 1, 1};
    if (m->find("color") && !num_list(m->find("color"), col, 4)) { error = "r3d_model(): prim " + std::to_string(pi) + "'s color wants 4 numbers"; return Value::nil(); }
    for (int i = 0; i < 4; ++i) pr.col[i] = (float)col[i];
    const auto P = bytes_member(m, "positions");
    const auto N = bytes_member(m, "normals");
    const auto U = bytes_member(m, "uvs");
    const auto J = bytes_member(m, "joints");
    const auto W = bytes_member(m, "weights");
    const auto I = bytes_member(m, "indices");
    const auto Cc = bytes_member(m, "colors");
    const auto Mt = bytes_member(m, "mats");
    if (!P || P->size() % 12) { error = "r3d_model(): prim " + std::to_string(pi) + " needs positions (float32 xyz)"; return Value::nil(); }
    const size_t nv = P->size() / 12;
    if (N && N->size() != nv * 12) { error = "r3d_model(): prim " + std::to_string(pi) + "'s normals are not one a vertex"; return Value::nil(); }
    if (U && U->size() != nv * 8) { error = "r3d_model(): prim " + std::to_string(pi) + "'s uvs are not one a vertex"; return Value::nil(); }
    if (pr.skinned && (!J || !W || J->size() != nv * 16 || W->size() != nv * 16 || md->joints.empty())) {
      error = "r3d_model(): prim " + std::to_string(pi) + " is skinned but lacks joints and weights (4 a vertex) or the model its joints";
      return Value::nil();
    }
    if (Cc && Cc->size() != nv * 12) { error = "r3d_model(): prim " + std::to_string(pi) + "'s colors are not one a vertex (float32 r g b)"; return Value::nil(); }
    if (Mt && Mt->size() != nv * 8) { error = "r3d_model(): prim " + std::to_string(pi) + "'s mats are not one a vertex (float32 shine glow)"; return Value::nil(); }
    // a sculpted part (vertex colours, no texture) gets the fill's fine relief, as the sculptor's own meshes do
    pr.relief = Cc.has_value() && !pr.tex;
    pr.v.resize(nv);
    for (size_t i = 0; i < nv; ++i) {
      auto& V = pr.v[i];
      for (int k = 0; k < 3; ++k) { V.p[k] = f32_at(*P, i * 3 + k); V.n[k] = N ? f32_at(*N, i * 3 + k) : 0.0f; }
      V.uv[0] = U ? f32_at(*U, i * 2) : 0.0f; V.uv[1] = U ? f32_at(*U, i * 2 + 1) : 0.0f;
      if (Cc) for (int k = 0; k < 3; ++k) V.c[k] = f32_at(*Cc, i * 3 + k);
      if (Mt) { V.shine = f32_at(*Mt, i * 2); V.glow = f32_at(*Mt, i * 2 + 1); }
      for (int k = 0; k < 4; ++k) {
        const uint32_t j = pr.skinned ? u32_at(*J, i * 4 + k) : 0;
        if (pr.skinned && j >= md->joints.size()) { error = "r3d_model(): prim " + std::to_string(pi) + " names joint " + std::to_string(j) + " of " + std::to_string(md->joints.size()); return Value::nil(); }
        V.j[k] = (uint8_t)j;
        V.w[k] = pr.skinned ? f32_at(*W, i * 4 + k) : (k == 0 ? 1.0f : 0.0f);
      }
      if (pr.skinned) {   // weights that do not add to 1 are made to
        const float s = V.w[0] + V.w[1] + V.w[2] + V.w[3];
        if (s > 1e-6f) for (int k = 0; k < 4; ++k) V.w[k] /= s;
      }
    }
    if (I) {
      if (I->size() % 12) { error = "r3d_model(): prim " + std::to_string(pi) + "'s indices are not whole triangles"; return Value::nil(); }
      pr.idx.resize(I->size() / 4);
      for (size_t i = 0; i < pr.idx.size(); ++i) {
        pr.idx[i] = u32_at(*I, i);
        if (pr.idx[i] >= nv) { error = "r3d_model(): prim " + std::to_string(pi) + " has an index past its vertices"; return Value::nil(); }
      }
    } else {
      if (nv % 3) { error = "r3d_model(): prim " + std::to_string(pi) + " has no indices and not whole triangles"; return Value::nil(); }
      for (uint32_t i = 0; i < nv; ++i) pr.idx.push_back(i);
    }
    // no normals: each vertex the mean of its triangles' faces
    if (!N) {
      for (size_t i = 0; i + 2 < pr.idx.size(); i += 3) {
        auto& A = pr.v[pr.idx[i]]; auto& B = pr.v[pr.idx[i + 1]]; auto& C = pr.v[pr.idx[i + 2]];
        const Vec fn = vcross(vsub({B.p[0], B.p[1], B.p[2]}, {A.p[0], A.p[1], A.p[2]}), vsub({C.p[0], C.p[1], C.p[2]}, {A.p[0], A.p[1], A.p[2]}));
        for (auto* V : {&A, &B, &C}) { V->n[0] += (float)fn.x; V->n[1] += (float)fn.y; V->n[2] += (float)fn.z; }
      }
      for (auto& V : pr.v) { const Vec n = vnorm({V.n[0], V.n[1], V.n[2]}); V.n[0] = (float)n.x; V.n[1] = (float)n.y; V.n[2] = (float)n.z; }
    }
    tris += (int64_t)pr.idx.size() / 3;
    md->prims.push_back(std::move(pr));
    ++pi;
  }
  // its size in our space at rest, for culling
  {
    std::vector<double> world((size_t)nn * 12);
    for (int i : md->order) {
      double L[12];
      trs_matrix(md->nodes[(size_t)i].t, md->nodes[(size_t)i].r, md->nodes[(size_t)i].s, L);
      if (md->nodes[(size_t)i].parent < 0) std::memcpy(&world[(size_t)i * 12], L, sizeof L);
      else m34_mul(&world[(size_t)md->nodes[(size_t)i].parent * 12], L, &world[(size_t)i * 12]);
    }
    for (const auto& pr : md->prims)
      for (const auto& V : pr.v) {
        Vec p{V.p[0], V.p[1], V.p[2]};
        if (!pr.skinned) p = m34_point(&world[(size_t)pr.node * 12], p);
        else { double M[12]; m34_mul(&world[(size_t)md->joints[V.j[0]] * 12], &md->inv_bind[(size_t)V.j[0] * 12], M); p = m34_point(M, p); }
        p = m34_point(md->root, p);
        md->radius = std::max(md->radius, std::hypot(p.x, p.y));
        md->height = std::max(md->height, p.z);
      }
  }
  auto out = std::make_shared<MapObj>();
  out->put("nodes", Value::integer(nn));
  out->put("joints", Value::integer((int64_t)md->joints.size()));
  out->put("tris", Value::integer(tris));
  out->put("height", Value::real(md->height));
  out->put("radius", Value::real(md->radius));
  t->models[args[1].as_int()] = std::move(md);
  return Value::object(Type::Map, out);
}

Value bi_r3d_anim(Vm&, std::vector<Value>& args, std::string& error) {
  auto t = get_target(args[0], error, "r3d_anim");
  if (!t) return Value::nil();
  if (!want_number(args[1], error, "r3d_anim") || !want_number(args[2], error, "r3d_anim") || !want_list(args[3], error, "r3d_anim")) return Value::nil();
  auto it = t->models.find(args[1].as_int());
  if (it == t->models.end()) { error = "r3d_anim(): no model " + std::to_string(args[1].as_int()); return Value::nil(); }
  R3dModel& md = *it->second;
  R3dModel::Anim an;
  size_t ci = 0;
  for (const Value& cv : args[3].list_items()) {
    if (cv.type != Type::Map) { error = "r3d_anim(): channel " + std::to_string(ci) + " is not a map"; return Value::nil(); }
    auto* m = static_cast<MapObj*>(cv.obj.get());
    R3dModel::Channel c;
    double nodev = -1;
    field(m, "node", nodev);
    c.node = (int)nodev;
    if (c.node < 0 || (size_t)c.node >= md.nodes.size()) { error = "r3d_anim(): channel " + std::to_string(ci) + "'s node is out of range"; return Value::nil(); }
    Value* pv = m->find("path");
    const std::string path = pv && pv->type == Type::Str ? pv->str() : "";
    if (path == "translation") c.path = 0; else if (path == "rotation") c.path = 1; else if (path == "scale") c.path = 2;
    else { error = "r3d_anim(): channel " + std::to_string(ci) + "'s path \"" + path + "\" (translation, rotation or scale)"; return Value::nil(); }
    Value* iv = m->find("interp");
    const std::string interp = iv && iv->type == Type::Str ? iv->str() : "LINEAR";
    c.interp = interp == "STEP" ? 1 : interp == "CUBICSPLINE" ? 2 : 0;
    const auto T = bytes_member(m, "times");
    const auto V = bytes_member(m, "vals");
    if (!T || !V || T->size() % 4 || V->size() % 4) { error = "r3d_anim(): channel " + std::to_string(ci) + " needs times and vals (float32 bytes)"; return Value::nil(); }
    const size_t keys = T->size() / 4, n = c.path == 1 ? 4 : 3, per = c.interp == 2 ? 3 : 1;
    if (V->size() / 4 != keys * n * per) { error = "r3d_anim(): channel " + std::to_string(ci) + " has " + std::to_string(V->size() / 4) + " values for " + std::to_string(keys) + " keys"; return Value::nil(); }
    c.times.resize(keys);
    for (size_t k = 0; k < keys; ++k) { c.times[k] = f32_at(*T, k); an.duration = std::max(an.duration, (double)c.times[k]); }
    c.vals.resize(V->size() / 4);
    for (size_t k = 0; k < c.vals.size(); ++k) c.vals[k] = f32_at(*V, k);
    an.ch.push_back(std::move(c));
    ++ci;
  }
  const double dur = an.duration;
  md.anims[args[2].as_int()] = std::move(an);
  return Value::real(dur);
}

// ---- sprite frames ---------------------------------------------------------------------------------------------
//
//   r3d_sprite_set(t, id, views) -> id
//       a sprite frame for r3d_draw_tex's things: views is one map (seen the same from everywhere) or eight
//       (rotations 1..8), each {tex (an id given to r3d_texture), flip, left, top} - the picture, mirrored or not,
//       and its origin (Doom's left and top offsets, in the picture's own texels).

Value bi_r3d_sprite_set(Vm&, std::vector<Value>& args, std::string& error) {
  auto t = get_target(args[0], error, "r3d_sprite_set");
  if (!t) return Value::nil();
  if (!want_number(args[1], error, "r3d_sprite_set") || !want_list(args[2], error, "r3d_sprite_set")) return Value::nil();
  const int64_t id = args[1].as_int();
  R3dSet set;
  for (const Value& vv : args[2].list_items()) {
    if (vv.type != Type::Map) { error = "r3d_sprite_set(): each view is a map {tex, flip, left, top}"; return Value::nil(); }
    auto* m = static_cast<MapObj*>(vv.obj.get());
    R3dView v;
    double texv = 0;
    if (!field(m, "tex", texv) || !field(m, "left", v.left) || !field(m, "top", v.top)) {
      error = "r3d_sprite_set(): each view needs numbers tex, left and top";
      return Value::nil();
    }
    v.tex = (int64_t)texv;
    if (!tex_of(*t, v.tex)) { error = "r3d_sprite_set(): texture " + std::to_string(v.tex) + " was not given to r3d_texture"; return Value::nil(); }
    if (Value* f = m->find("flip")) v.flip = f->type == Type::Bool ? f->b : (f->is_number() && f->as_double() != 0);
    set.views.push_back(v);
  }
  if (set.views.size() != 1 && set.views.size() != 8) { error = "r3d_sprite_set(): a sprite frame has 1 view or 8, not " + std::to_string(set.views.size()); return Value::nil(); }
  t->sets[id] = std::move(set);
  return Value::integer(id);
}

Value bi_r3d_draw_tex(Vm&, std::vector<Value>& args, std::string& error) {
  auto t = get_target(args[0], error, "r3d_draw_tex");
  if (!t) return Value::nil();
  if (!want_list(args[1], error, "r3d_draw_tex")) return Value::nil();
  double fov = 90.0;
  if (args.size() > 2) {
    if (!want_number(args[2], error, "r3d_draw_tex")) return Value::nil();
    fov = args[2].as_double();
  }
  if (!(fov > 1.0 && fov < 179.0)) { error = "r3d_draw_tex(): fov_deg must be between 1 and 179"; return Value::nil(); }
  const double focal = (t->w * 0.5) / std::tan(fov * 3.14159265358979323846 / 360.0);
  t->focal = focal;
  t->tris = t->filled = t->clipped = t->pixels = 0;
  t->pitch = 0;
  // With a camera {x, y, z, angle} (Doom's map axes: x east, y north, z up, angle in degrees from east), the rows
  // are in the world and are turned to the camera here, each frame, from geometry worked out once a tic.
  bool world = false;
  double cxw = 0, cyw = 0, czw = 0, ca = 1, sa = 0, cp = 1, sp = 0;
  if (args.size() > 3 && args[3].type != Type::Nil) {
    if (args[3].type != Type::Map) { error = "r3d_draw_tex(): the camera wants a map {x, y, z, angle}"; return Value::nil(); }
    auto* cm = static_cast<MapObj*>(args[3].obj.get());
    double ang = 0;
    if (!field(cm, "x", cxw) || !field(cm, "y", cyw) || !field(cm, "z", czw) || !field(cm, "angle", ang)) {
      error = "r3d_draw_tex(): the camera wants numbers x, y, z and angle";
      return Value::nil();
    }
    const double r = ang * 3.14159265358979323846 / 180.0;
    ca = std::cos(r);
    sa = std::sin(r);
    // looking up (positive) or down: degrees, a little short of straight up or down
    double pitch = 0;
    field(cm, "pitch", pitch);
    pitch = std::max(-85.0, std::min(85.0, pitch)) * 3.14159265358979323846 / 180.0;
    cp = std::cos(pitch);
    sp = std::sin(pitch);
    t->pitch = pitch;
    world = true;
  }
  // a point (or, without the eye's place, a direction) of the world as the camera sees it: x right, y up, z ahead
  auto to_cam = [&](double ex, double ey, double ez) {
    const double x = ex * sa - ey * ca, f = ex * ca + ey * sa;
    return Vec{x, ez * cp - f * sp, ez * sp + f * cp};
  };
  // the lights, turned to the camera with the scene (none for a draw without one: its rows are camera space already
  // and so would the lights have to be); those wholly behind the eye are left out
  t->lights_cam.clear();
  if (world)
    for (const R3dLight& L : t->lights_world) {
      const Vec c = to_cam(L.x - cxw, L.y - cyw, L.z - czw);
      if (c.z < -L.radius) continue;
      t->lights_cam.push_back({c.x, c.y, c.z, L.r, L.g, L.b, L.radius});
      if (t->lights_cam.size() >= 255) break;
    }
  static const std::string kx[3] = {"x0", "x1", "x2"}, ky[3] = {"y0", "y1", "y2"}, kz[3] = {"z0", "z1", "z2"};
  static const std::string ku[3] = {"u0", "u1", "u2"}, kv[3] = {"v0", "v1", "v2"};
  static const std::string ktex = "tex", klight = "light", kflags = "flags";
  // Opaque triangles first, then masked ones (their transparent texels show what is already there), then fuzz.
  std::vector<STri> tris[3];
  size_t at = 0;
  for (const Value& row : args[1].list_items()) {
    if (row.type != Type::Map) { error = "r3d_draw_tex(): row " + std::to_string(at) + " is a " + type_name(row.type) + ", not a map"; return Value::nil(); }
    auto* m = static_cast<MapObj*>(row.obj.get());
    TV v[3];
    for (int i = 0; i < 3; ++i) {
      if (!field(m, kx[i], v[i].x) || !field(m, ky[i], v[i].y) || !field(m, kz[i], v[i].z) || !field(m, ku[i], v[i].u) ||
          !field(m, kv[i], v[i].v)) {
        error = "r3d_draw_tex(): row " + std::to_string(at) + " needs numbers x0 y0 z0 u0 v0 .. x2 y2 z2 u2 v2";
        return Value::nil();
      }
    }
    if (world)
      for (int i = 0; i < 3; ++i) {
        const double dx = v[i].x - cxw, dy = v[i].y - cyw;
        const Vec c = to_cam(dx, dy, v[i].z - czw);
        v[i] = {c.x, c.y, c.z, v[i].u, v[i].v};
      }
    double texv = 0, lightv = 255, flagsv = 0;
    if (!field(m, ktex, texv)) { error = "r3d_draw_tex(): row " + std::to_string(at) + " needs a number tex"; return Value::nil(); }
    field(m, klight, lightv);
    field(m, kflags, flagsv);
    auto tx = tex_of(*t, (int64_t)texv);
    if (!tx) { error = "r3d_draw_tex(): row " + std::to_string(at) + " names texture " + std::to_string((int64_t)texv) + ", which r3d_texture was not given"; return Value::nil(); }
    const int flags = (int)flagsv;
    ++t->tris;
    clip_project(*t, v, focal, tx.get(), (float)(std::min(255.0, std::max(0.0, lightv)) / 255.0), flags,
                 &tris[(flags & 8) ? 2 : (flags & 1) ? 1 : 0], &t->clipped);
    ++at;
  }
  if (args.size() > 4 && args[4].type != Type::Nil) {
    if (!world) { error = "r3d_draw_tex(): things want a camera"; return Value::nil(); }
    if (!want_list(args[4], error, "r3d_draw_tex")) return Value::nil();
    constexpr double kPi = 3.14159265358979323846;
    const double half_fov = std::atan(t->w * 0.5 / focal);
    static const std::string kset = "set", kangle = "angle", kz0 = "z", kx0 = "x", ky0 = "y";
    size_t ti = 0;
    for (const Value& row : args[4].list_items()) {
      if (row.type != Type::Map) { error = "r3d_draw_tex(): thing " + std::to_string(ti) + " is not a map"; return Value::nil(); }
      auto* m = static_cast<MapObj*>(row.obj.get());
      double setv = -1, meshv = -1, modelv = -1, X = 0, Y = 0, Z = 0, ang = 0, lightv = 255, flagsv = 0;
      field(m, kset, setv);
      field(m, "mesh", meshv);
      field(m, "model", modelv);
      if ((setv < 0 && meshv < 0 && modelv < 0) || !field(m, kx0, X) || !field(m, ky0, Y) || !field(m, kz0, Z)) {
        error = "r3d_draw_tex(): thing " + std::to_string(ti) + " needs a set, a mesh or a model, and numbers x, y and z";
        return Value::nil();
      }
      field(m, kangle, ang);
      field(m, klight, lightv);
      field(m, kflags, flagsv);
      ++ti;
      const float light = (float)(std::min(255.0, std::max(0.0, lightv)) / 255.0);
      const int tflags = (int)flagsv & (2 | 8);
      const double dx = X - cxw, dy = Y - cyw;
      const double fwd = dx * ca + dy * sa, side = dx * sa - dy * ca;
      if (modelv >= 0) {
        auto mit = t->models.find((int64_t)modelv);
        if (mit == t->models.end()) { error = "r3d_draw_tex(): thing " + std::to_string(ti - 1) + " names model " + std::to_string((int64_t)modelv) + ", which r3d_model was not given"; return Value::nil(); }
        const R3dModel& md = *mit->second;
        double scale = 1.0;
        field(m, "scale", scale);
        const double reach = (md.radius + md.height) * scale;
        if (fwd < -reach) continue;
        if (fwd > 0 && std::fabs(side) - reach > std::tan(half_fov) * fwd + reach) continue;
        // the pose: its animation at its time, blended with a second
        double animv = -1, tm = 0, anim2v = -1, tm2 = 0, mix = 0;
        field(m, "anim", animv); field(m, "time", tm); field(m, "anim2", anim2v); field(m, "time2", tm2); field(m, "mix", mix);
        auto flag_of = [&](const char* k) { Value* v = m->find(k); return v && v->type == Type::Bool && v->b; };
        const bool hold = flag_of("hold"), hold2 = flag_of("hold2");
        auto find_anim = [&](double id) -> const R3dModel::Anim* {
          if (id < 0) return nullptr;
          auto a = md.anims.find((int64_t)id);
          return a == md.anims.end() ? nullptr : &a->second;
        };
        std::vector<R3dModel::Node> posed, posed2;
        pose_nodes(md, find_anim(animv), tm, hold, &posed);
        if (anim2v >= 0 && mix > 0) {
          pose_nodes(md, find_anim(anim2v), tm2, hold2, &posed2);
          const double k = std::min(1.0, mix);
          for (size_t i = 0; i < posed.size(); ++i) {
            for (int e = 0; e < 3; ++e) { posed[i].t[e] += (posed2[i].t[e] - posed[i].t[e]) * k; posed[i].s[e] += (posed2[i].s[e] - posed[i].s[e]) * k; }
            double q[4];
            nlerp_q(posed[i].r, posed2[i].r, k, q);
            std::memcpy(posed[i].r, q, sizeof q);
          }
        }
        // every node in the model's space, then each joint's skin matrix, all into ours (axes, scale, where the
        // thing stands and which way it faces)
        const size_t nn = md.nodes.size();
        std::vector<double> world(nn * 12);
        for (int i : md.order) {
          double L[12];
          trs_matrix(posed[(size_t)i].t, posed[(size_t)i].r, posed[(size_t)i].s, L);
          if (md.nodes[(size_t)i].parent < 0) std::memcpy(&world[(size_t)i * 12], L, sizeof L);
          else m34_mul(&world[(size_t)md.nodes[(size_t)i].parent * 12], L, &world[(size_t)i * 12]);
        }
        // which way it faces (angle), and, for what is not stood on the ground (a weapon in the eye's hand), how far it
        // tips its nose up (pitch) and rolls to its right (roll), in degrees: Rz(angle) Ry(-pitch) Rx(roll)
        double pitchv = 0, rollv = 0;
        field(m, "pitch", pitchv);
        field(m, "roll", rollv);
        const double th = ang * kPi / 180.0, ct = std::cos(th), st = std::sin(th);
        const double ph = -pitchv * kPi / 180.0, cph = std::cos(ph), sph = std::sin(ph);
        const double rh = rollv * kPi / 180.0, crh = std::cos(rh), srh = std::sin(rh);
        // Rz * Ry * Rx, row by row
        const double R00 = ct * cph, R01 = ct * sph * srh - st * crh, R02 = ct * sph * crh + st * srh;
        const double R10 = st * cph, R11 = st * sph * srh + ct * crh, R12 = st * sph * crh - ct * srh;
        const double R20 = -sph, R21 = cph * srh, R22 = cph * crh;
        const double place[12] = {R00 * scale, R01 * scale, R02 * scale, X, R10 * scale, R11 * scale, R12 * scale, Y,
                                  R20 * scale, R21 * scale, R22 * scale, Z};
        double to_world[12];
        m34_mul(place, md.root, to_world);
        std::vector<double> skin(md.joints.size() * 12);
        for (size_t j = 0; j < md.joints.size(); ++j) {
          double M[12];
          m34_mul(&world[(size_t)md.joints[j] * 12], &md.inv_bind[j * 12], M);
          m34_mul(to_world, M, &skin[j * 12]);
        }
        for (const auto& pr : md.prims) {
          double node_m[12];
          if (!pr.skinned) m34_mul(to_world, &world[(size_t)pr.node * 12], node_m);
          std::vector<TV> cam(pr.v.size());
          std::vector<Vec> wp(pr.v.size());
          for (size_t i = 0; i < pr.v.size(); ++i) {
            const auto& V = pr.v[i];
            double M[12];
            if (pr.skinned) {
              for (int e = 0; e < 12; ++e) M[e] = 0;
              for (int k = 0; k < 4; ++k) {
                if (V.w[k] == 0) continue;
                const double* S = &skin[(size_t)V.j[k] * 12];
                for (int e = 0; e < 12; ++e) M[e] += S[e] * V.w[k];
              }
            } else {
              std::memcpy(M, node_m, sizeof M);
            }
            const Vec W = m34_point(M, {V.p[0], V.p[1], V.p[2]});
            const Vec N = vnorm(m34_dir(M, {V.n[0], V.n[1], V.n[2]}));
            wp[i] = W;
            const double ex = W.x - cxw, ey = W.y - cyw;
            const Vec c = to_cam(ex, ey, W.z - czw);
            TV tv{c.x, c.y, c.z, V.uv[0], V.uv[1]};
            tv.a[0] = pr.col[0] * V.c[0]; tv.a[1] = pr.col[1] * V.c[1]; tv.a[2] = pr.col[2] * V.c[2];
            tv.shine = V.shine; tv.glow = V.glow;
            if (pr.relief) { tv.a[6] = V.p[0]; tv.a[7] = V.p[1]; tv.a[8] = V.p[2]; }
            const Vec nc = to_cam(N.x, N.y, N.z);
            tv.a[3] = nc.x; tv.a[4] = nc.y; tv.a[5] = nc.z;
            cam[i] = tv;
          }
          const R3dTex* tex = pr.tex.get();
          // texture coordinates are 0..1 across the picture: in its texels for the fill
          if (tex) for (auto& c : cam) { c.u *= tex->mips[0].w; c.v *= tex->mips[0].h; }
          auto& out = tris[(tflags & 8) ? 2 : 0];
          for (size_t i = 0; i + 2 < pr.idx.size(); i += 3) {
            const uint32_t a = pr.idx[i], b = pr.idx[i + 1], c = pr.idx[i + 2];
            const Vec fn = vcross(vsub(wp[b], wp[a]), vsub(wp[c], wp[a]));
            if (vdot(fn, vsub({cxw, cyw, czw}, wp[a])) <= 0) continue;
            const TV v3[3] = {cam[a], cam[b], cam[c]};
            ++t->tris;
            clip_project(*t, v3, focal, tex, light, tflags | 64, &out, &t->clipped);
          }
        }
        continue;
      }
      if (meshv >= 0) {
        auto mit = t->meshes.find((int64_t)meshv);
        if (mit == t->meshes.end()) { error = "r3d_draw_tex(): thing " + std::to_string(ti - 1) + " names mesh " + std::to_string((int64_t)meshv) + ", which r3d_mesh was not given"; return Value::nil(); }
        const R3dMesh& mesh = *mit->second;
        const double reach = mesh.radius + mesh.height;
        if (fwd < -reach) continue;
        if (fwd > 0 && std::fabs(side) - reach > std::tan(half_fov) * fwd + reach) continue;
        // the bones' poses: 3x4 matrices row by row (the model's space from each bone's), identity if not given
        std::vector<double> bm((size_t)mesh.bones * 12, 0.0);
        for (int b = 0; b < mesh.bones; ++b) { bm[(size_t)b * 12] = 1; bm[(size_t)b * 12 + 5] = 1; bm[(size_t)b * 12 + 10] = 1; }
        if (Value* bv = m->find("bones")) {
          if (bv->type != Type::List) { error = "r3d_draw_tex(): thing " + std::to_string(ti - 1) + "'s bones wants a list of numbers"; return Value::nil(); }
          const auto& L = bv->list_items();
          for (size_t i = 0; i < L.size() && i < bm.size(); ++i) {
            if (!L[i].is_number()) { error = "r3d_draw_tex(): thing " + std::to_string(ti - 1) + "'s bones wants numbers"; return Value::nil(); }
            bm[i] = L[i].as_double();
          }
        }
        double scale = 1.0;
        field(m, "scale", scale);
        const double th = ang * kPi / 180.0, ct = std::cos(th), st = std::sin(th);
        std::vector<TV> cam(mesh.v.size());
        std::vector<Vec> wp(mesh.v.size());
        // each bone's skin matrix: from the model at rest to the bone posed (posed x inverse rest)
        std::vector<double> skin((size_t)mesh.bones * 12);
        for (int b = 0; b < mesh.bones; ++b) m34_mul(&bm[(size_t)b * 12], &mesh.inv_rest[(size_t)b * 12], &skin[(size_t)b * 12]);
        for (size_t i = 0; i < mesh.v.size(); ++i) {
          const auto& V = mesh.v[i];
          const double* A = &skin[(size_t)V.bone[0] * 12];
          const double* Bm = &skin[(size_t)V.bone[1] * 12];
          const double w0 = V.w0, w1 = 1.0 - V.w0;
          double M[12];
          for (int e = 0; e < 12; ++e) M[e] = A[e] * w0 + Bm[e] * w1;
          const double px = M[0] * V.x + M[1] * V.y + M[2] * V.z + M[3];
          const double py = M[4] * V.x + M[5] * V.y + M[6] * V.z + M[7];
          const double pz = M[8] * V.x + M[9] * V.y + M[10] * V.z + M[11];
          const double nx = M[0] * V.nx + M[1] * V.ny + M[2] * V.nz;
          const double ny = M[4] * V.nx + M[5] * V.ny + M[6] * V.nz;
          const double nz = M[8] * V.nx + M[9] * V.ny + M[10] * V.nz;
          const Vec W{X + (px * ct - py * st) * scale, Y + (px * st + py * ct) * scale, Z + pz * scale};
          const Vec N = vnorm({nx * ct - ny * st, nx * st + ny * ct, nz});
          wp[i] = W;
          const double ex = W.x - cxw, ey = W.y - cyw;
          const Vec c = to_cam(ex, ey, W.z - czw);
          TV tv{c.x, c.y, c.z, 0, 0};
          tv.a[0] = V.r; tv.a[1] = V.g; tv.a[2] = V.b;
          // the normal in camera space
          const Vec nc = to_cam(N.x, N.y, N.z);
          tv.a[3] = nc.x; tv.a[4] = nc.y; tv.a[5] = nc.z;
          tv.a[6] = V.x; tv.a[7] = V.y; tv.a[8] = V.z;
          tv.shine = V.shine; tv.glow = V.glow;
          cam[i] = tv;
        }
        auto& out = tris[(tflags & 8) ? 2 : 0];
        for (size_t i = 0; i + 2 < mesh.idx.size(); i += 3) {
          const uint32_t a = mesh.idx[i], b = mesh.idx[i + 1], c = mesh.idx[i + 2];
          const Vec fn = vcross(vsub(wp[b], wp[a]), vsub(wp[c], wp[a]));
          if (vdot(fn, vsub({cxw, cyw, czw}, wp[a])) <= 0) continue;
          const TV v3[3] = {cam[a], cam[b], cam[c]};
          ++t->tris;
          clip_project(*t, v3, focal, nullptr, light, tflags, &out, &t->clipped);
        }
        continue;
      }
      auto it = t->sets.find((int64_t)setv);
      if (it == t->sets.end()) { error = "r3d_draw_tex(): thing " + std::to_string(ti - 1) + " names sprite set " + std::to_string((int64_t)setv) + ", which r3d_sprite_set was not given"; return Value::nil(); }
      const R3dSet& set = it->second;
      // well behind the eye, or well off either side of the view: nothing to draw
      if (fwd < -128.0) continue;
      if (fwd > 0 && std::fabs(side) - 128.0 > std::tan(half_fov) * fwd + 128.0) continue;
      // the picture, upright, across the line of sight: the view for where the camera stands
      const R3dView* view = &set.views[0];
      if (set.views.size() == 8) {
        const double toward = std::atan2(cyw - Y, cxw - X) * 180.0 / kPi;
        double rel = toward - ang + 22.5;
        rel -= 360.0 * std::floor(rel / 360.0);
        view = &set.views[(size_t)std::min(7, (int)(rel / 45.0))];
      }
      auto tx = tex_of(*t, view->tex);
      if (!tx) continue;
      const double W = tx->mips[0].w / tx->scale, H = tx->mips[0].h / tx->scale;
      const double h0 = -view->left, h1 = W - view->left;
      const double ax = X + sa * h0, ay = Y - ca * h0, bx = X + sa * h1, by = Y - ca * h1;
      const double zb = Z + view->top - H, zt = Z + view->top;
      const double ul = view->flip ? W : 0, ur = view->flip ? 0 : W;
      auto cam_of = [&](double wx, double wy, double wz, double u, double vv) {
        const double ex = wx - cxw, ey = wy - cyw;
        const Vec c = to_cam(ex, ey, wz - czw);
        return TV{c.x, c.y, c.z, u, vv};
      };
      const TV A = cam_of(ax, ay, zb, ul, H), B = cam_of(bx, by, zb, ur, H), C = cam_of(bx, by, zt, ur, 0), D = cam_of(ax, ay, zt, ul, 0);
      const int sflags = 1 | 4 | tflags;
      auto& out = tris[(sflags & 8) ? 2 : 1];
      const TV q0[3] = {A, B, C}, q1[3] = {A, C, D};
      t->tris += 2;
      clip_project(*t, q0, focal, tx.get(), light, sflags, &out, &t->clipped);
      clip_project(*t, q1, focal, tx.get(), light, sflags, &out, &t->clipped);
    }
  }
  // The rows of the target in bands, one band per task: every triangle is filled band by band, so no two threads
  // write a pixel, and each pass (opaque, masked, fuzz) is finished before the next starts.
  const int band = 16;
  const size_t nb = (size_t)((t->h + band - 1) / band);
  std::vector<int64_t> px(SqlThreadPool::instance().num_workers() + 1, 0);
  for (int pass = 0; pass < 3; ++pass) {
    const std::vector<STri>& list = tris[pass];
    if (list.empty()) continue;
    t->filled += (int64_t)list.size();
    // each task counts its pixels on its own stack and adds them once: the counters side by side in px share a
    // cache line, and an increment there per pixel bounced that line between the cores (a third of the fill)
    auto run = [&](size_t b0, size_t b1, size_t tid) {
      int64_t n = 0;
      for (size_t b = b0; b < b1; ++b) {
        const int ya = (int)b * band, yb = std::min(t->h, ya + band);
        for (const STri& s : list)
          if (s.y1 >= ya && s.y0 < yb) fill_tex_rows(*t, s, ya, yb, &n);
      }
      px[std::min(tid, px.size() - 1)] += n;
    };
    SqlThreadPool::instance().for_chunks(nb, 1, (size_t)t->w * (size_t)t->h, false, run);
  }
  for (int64_t p : px) t->pixels += p;
  return Value::integer(t->filled);
}

Value bi_r3d_sky(Vm&, std::vector<Value>& args, std::string& error) {
  auto t = get_target(args[0], error, "r3d_sky");
  if (!t) return Value::nil();
  for (int i = 1; i <= 3; ++i) if (!want_number(args[(size_t)i], error, "r3d_sky")) return Value::nil();
  auto tx = tex_of(*t, args[1].as_int());
  if (!tx) { error = "r3d_sky(): texture " + std::to_string(args[1].as_int()) + " was not given to r3d_texture"; return Value::nil(); }
  const double yaw = args[2].as_double(), fov = args[3].as_double();
  if (!(fov > 1.0 && fov < 179.0)) { error = "r3d_sky(): fov_deg must be between 1 and 179"; return Value::nil(); }
  const double focal = (t->w * 0.5) / std::tan(fov * 3.14159265358979323846 / 360.0);
  const R3dTex::Level& L = tx->mips[0];
  const double sc = tx->scale;
  // Doom's: 1024 sky columns round the horizon (a 256-wide texture four times), each screen row the texture row
  // it would be on Doom's 200-row view (the horizon at row 100), the rows below the texture its last one.
  std::vector<double> col((size_t)t->w);
  for (int x = 0; x < t->w; ++x) {
    const double ang = -yaw + std::atan2(x + 0.5 - t->w * 0.5, focal) * 57.29577951308232;
    col[(size_t)x] = ang / 360.0 * 1024.0 * L.w / (256.0 * sc) * sc;
  }
  // Only the pixels nothing has been drawn on (depth 0): drawn after the scene, the sky costs what shows of it,
  // not the whole screen (6 ms a frame at 1280x960 when it went first), in bands across the pool as the fill is.
  // each screen row's height on Doom's 200-row view of the sky (its horizon at row 100), looking up or down by the
  // last draw's pitch: the elevation of the row's ray, turned back to where that row would be with the view level
  const double fy = focal * t->y_scale;
  const double rows_per_unit = 200.0 / t->h * fy;   // Doom-rows per unit of tan(elevation)
  std::vector<double> vrow((size_t)t->h);
  for (int y = 0; y < t->h; ++y) {
    const double el = std::atan2(t->h * 0.5 - (y + 0.5), fy) + t->pitch;
    vrow[(size_t)y] = 100.0 - std::tan(std::max(-1.45, std::min(1.45, el))) * rows_per_unit;
  }
  // Past the picture's top (looking up, which Doom never could) and its bottom: the picture fades into the mean
  // colour of its top (bottom) eighth, from a quarter inside the edge to half the picture's height past it, and is
  // that colour beyond. Held at
  // the edge row, every column was its own top texel straight up the screen: vertical stripes.
  auto mean_rows = [&](int r0, int r1) {
    uint64_t R = 0, G = 0, B = 0, n = 0;
    for (int r = std::max(0, r0); r < std::min(L.h, r1); ++r)
      for (int x = 0; x < L.w; ++x) {
        const uint32_t p = L.px[(size_t)r * L.w + x];
        R += (p >> 16) & 255; G += (p >> 8) & 255; B += p & 255; ++n;
      }
    if (!n) return 0u;
    return (uint32_t)(((R / n) << 16) | ((G / n) << 8) | (B / n));
  };
  const int eighth = std::max(1, L.h / 8);
  const uint32_t cap_top = mean_rows(0, eighth), cap_bottom = mean_rows(L.h - eighth, L.h);
  const double fade = std::max(1.0, L.h * 0.5);
  auto mix = [](uint32_t a, uint32_t b, double f) {
    const double g = 1.0 - f;
    return ((uint32_t)(((a >> 16) & 255) * g + ((b >> 16) & 255) * f) << 16) |
           ((uint32_t)(((a >> 8) & 255) * g + ((b >> 8) & 255) * f) << 8) | (uint32_t)((a & 255) * g + (b & 255) * f);
  };
  // Doom's skies are streaked in their upper part (each column's colour run down), invisible at 320x200 and stripes
  // across a large screen: toward the top the picture blends into a copy of itself smoothed along each row (a box of
  // 1/16 of the picture's width), the mountains below it left as they are.
  // made once a texture (a running sum along each row) and kept with it
  if (tx->sky_smooth.px.size() != L.px.size()) {
    R3dTex::Level sm = L;
    const int rad = std::max(1, L.w / 32);
    const uint32_t n = (uint32_t)(2 * rad + 1);
    for (int r = 0; r < L.h; ++r) {
      const uint32_t* src = &L.px[(size_t)r * L.w];
      uint32_t* dst = &sm.px[(size_t)r * L.w];
      auto at = [&](int x) { return src[((x % L.w) + L.w) % L.w]; };
      uint32_t R = 0, G = 0, B = 0;
      for (int k = -rad; k <= rad; ++k) { const uint32_t p = at(k); R += (p >> 16) & 255; G += (p >> 8) & 255; B += p & 255; }
      for (int x = 0; x < L.w; ++x) {
        dst[x] = 0xFF000000u | ((R / n) << 16) | ((G / n) << 8) | (B / n);
        const uint32_t out = at(x - rad), in = at(x + rad + 1);
        R += ((in >> 16) & 255) - ((out >> 16) & 255);
        G += ((in >> 8) & 255) - ((out >> 8) & 255);
        B += (in & 255) - (out & 255);
      }
    }
    tx->sky_smooth = std::move(sm);
  }
  const R3dTex::Level& smooth = tx->sky_smooth;
  const double smooth_to = L.h * 0.6;   // from this row up the smoothed copy takes over, wholly by the top
  const bool bil = t->bilinear;
  R3dTarget& T = *t;
  auto rows = [&](size_t y0, size_t y1, size_t) {
    for (size_t y = y0; y < y1; ++y) {
      const double vr = vrow[y] * sc;
      double g = vr < smooth_to ? std::min(1.0, (smooth_to - vr) / smooth_to) : 0.0;
      g = g * g * (3.0 - 2.0 * g);
      const double v = std::min((double)L.h - 0.51, std::max(0.0, vr));
      // how far past an edge, as a smooth 0..1
      // the fade starts inside the picture (a quarter of it from the edge, where Doom's skies already streak: their
      // top rows are a column's colour run down) and ends half a picture past it
      double f = 0.0;
      uint32_t cap = cap_top;
      const double inner = L.h * 0.25;
      if (vr < inner) f = std::min(1.0, (inner - vr) / (inner + fade));
      else if (vr > L.h - 1 - inner) { f = std::min(1.0, (vr - (L.h - 1 - inner)) / (inner + fade)); cap = cap_bottom; }
      f = f * f * (3.0 - 2.0 * f);
      uint32_t* crow = &T.color[y * T.w];
      const float* drow = &T.depth[y * T.w];
      for (int x = 0; x < T.w; ++x) {
        if (drow[x] != 0.0f) continue;
        if (f >= 1.0) { crow[x] = cap; continue; }
        uint32_t p = sample(L, col[(size_t)x], v, false, bil) & 0xFFFFFF;
        if (g > 0.0) p = mix(p, sample(smooth, col[(size_t)x], v, false, bil) & 0xFFFFFF, g);
        crow[x] = f > 0.0 ? mix(p, cap, f) : p;
      }
    }
  };
  SqlThreadPool::instance().for_chunks((size_t)t->h, 32, (size_t)t->w * (size_t)t->h, false, rows);
  return args[0];
}

Value bi_r3d_blit(Vm&, std::vector<Value>& args, std::string& error) {
  auto t = get_target(args[0], error, "r3d_blit");
  if (!t) return Value::nil();
  for (int i = 1; i <= 6; ++i) if (!want_number(args[(size_t)i], error, "r3d_blit")) return Value::nil();
  auto tx = tex_of(*t, args[1].as_int());
  if (!tx) { error = "r3d_blit(): texture " + std::to_string(args[1].as_int()) + " was not given to r3d_texture"; return Value::nil(); }
  const double X = args[2].as_double(), Y = args[3].as_double(), Wd = args[4].as_double(), Hd = args[5].as_double();
  const double b = std::min(1.0, std::max(0.0, args[6].as_double() / 255.0));
  if (!(Wd > 0 && Hd > 0)) return args[0];
  const R3dTex::Level& L = tx->mips[0];
  const int x0 = std::max(0, (int)std::floor(X)), x1 = std::min(t->w - 1, (int)std::ceil(X + Wd));
  const int y0 = std::max(0, (int)std::floor(Y)), y1 = std::min(t->h - 1, (int)std::ceil(Y + Hd));
  for (int y = y0; y <= y1; ++y) {
    const double v = (y + 0.5 - Y) / Hd * L.h;
    if (v < 0 || v >= L.h) continue;
    uint32_t* crow = &t->color[(size_t)y * t->w];
    for (int x = x0; x <= x1; ++x) {
      const double u = (x + 0.5 - X) / Wd * L.w;
      if (u < 0 || u >= L.w) continue;
      const uint32_t p = sample(L, u, v, false, t->bilinear);
      if ((p >> 24) < 128) continue;
      crow[x] = ((uint32_t)(((p >> 16) & 255) * b) << 16) | ((uint32_t)(((p >> 8) & 255) * b) << 8) | (uint32_t)((p & 255) * b);
    }
  }
  return args[0];
}

// r3d_clear_depth(t) -> t: the depth buffer emptied, the colour left (a sky drawn first stays behind everything).
Value bi_r3d_clear_depth(Vm&, std::vector<Value>& args, std::string& error) {
  auto t = get_target(args[0], error, "r3d_clear_depth");
  if (!t) return Value::nil();
  std::fill(t->depth.begin(), t->depth.end(), 0.0f);
  return args[0];
}

// r3d_lights(t, rows) -> how many: the point lights for the draws that follow, each {x, y, z, radius, r, g, b} on the
// world's axes (as r3d_draw_tex's camera is), colour channels 0..1 and past (a share of the lit surface's own
// colour added at the light's heart). An empty list puts them out.
Value bi_r3d_lights(Vm&, std::vector<Value>& args, std::string& error) {
  auto t = get_target(args[0], error, "r3d_lights");
  if (!t) return Value::nil();
  if (!want_list(args[1], error, "r3d_lights")) return Value::nil();
  std::vector<R3dLight> out;
  size_t at = 0;
  for (const Value& row : args[1].list_items()) {
    if (row.type != Type::Map) { error = "r3d_lights(): light " + std::to_string(at) + " is a " + type_name(row.type) + ", not a map"; return Value::nil(); }
    auto* m = static_cast<MapObj*>(row.obj.get());
    R3dLight L{};
    if (!field(m, "x", L.x) || !field(m, "y", L.y) || !field(m, "z", L.z) || !field(m, "radius", L.radius) ||
        !field(m, "r", L.r) || !field(m, "g", L.g) || !field(m, "b", L.b)) {
      error = "r3d_lights(): light " + std::to_string(at) + " needs numbers x y z radius r g b";
      return Value::nil();
    }
    ++at;
    if (!(L.radius > 0)) continue;
    out.push_back(L);
  }
  t->lights_world = std::move(out);
  return Value::integer((int64_t)t->lights_world.size());
}

Value bi_r3d_options(Vm&, std::vector<Value>& args, std::string& error) {
  auto t = get_target(args[0], error, "r3d_options");
  if (!t) return Value::nil();
  if (args[1].type != Type::Map) { error = "r3d_options() wants a map of options"; return Value::nil(); }
  auto* m = static_cast<MapObj*>(args[1].obj.get());
  for (const auto& kv : m->entries) {
    const std::string& k = kv.first;
    const Value& v = kv.second;
    if (k == "bilinear" || k == "mipmaps") {
      if (v.type != Type::Bool) { error = "r3d_options(): " + k + " wants true or false"; return Value::nil(); }
      (k == "bilinear" ? t->bilinear : t->mipmaps) = v.b;
    } else if (k == "time") {
      if (!v.is_number()) { error = "r3d_options(): time wants a number (seconds)"; return Value::nil(); }
      t->time = v.as_double();
    } else if (k == "upscale") {
      if (!v.is_number() || (v.as_int() != 1 && v.as_int() != 2 && v.as_int() != 4)) { error = "r3d_options(): upscale wants 1, 2 or 4"; return Value::nil(); }
      t->upscale = (int)v.as_int();
    } else if (k == "y_scale" || k == "light_gain" || k == "light_lift" || k == "light_fall") {
      if (!v.is_number()) { error = "r3d_options(): " + k + " wants a number"; return Value::nil(); }
      const double d = v.as_double();
      if (k == "y_scale") { if (!(d > 0.1 && d < 10)) { error = "r3d_options(): y_scale out of range"; return Value::nil(); } t->y_scale = d; }
      else if (k == "light_gain") t->light_gain = d;
      else if (k == "light_lift") t->light_lift = d;
      else t->light_fall = d;
    } else {
      error = "r3d_options(): no option " + k + " (bilinear, mipmaps, upscale, time, y_scale, light_gain, light_lift, light_fall)";
      return Value::nil();
    }
  }
  return args[0];
}

}  // namespace

void install_raster3d_builtins(Vm& vm) {
  vm.define_native("r3d_texture", 6, 7, bi_r3d_texture);
  vm.define_native("r3d_draw_tex", 2, 5, bi_r3d_draw_tex);
  vm.define_native("r3d_sprite_set", 3, 3, bi_r3d_sprite_set);
  vm.define_native("r3d_mesh", 4, 4, bi_r3d_mesh);
  vm.define_native("r3d_texture_rgba", 5, 6, bi_r3d_texture_rgba);
  vm.define_native("r3d_model", 3, 3, bi_r3d_model);
  vm.define_native("r3d_mesh_data", 2, 2, bi_r3d_mesh_data);
  vm.define_native("r3d_anim", 4, 4, bi_r3d_anim);
  vm.define_native("r3d_sky", 4, 4, bi_r3d_sky);
  vm.define_native("r3d_blit", 7, 7, bi_r3d_blit);
  vm.define_native("r3d_options", 2, 2, bi_r3d_options);
  vm.define_native("r3d_clear_depth", 1, 1, bi_r3d_clear_depth);
  vm.define_native("r3d_lights", 2, 2, bi_r3d_lights);
  vm.define_native("r3d_target", 2, 2, bi_r3d_target);
  vm.define_native("r3d_clear", 1, 2, bi_r3d_clear);
  vm.define_native("r3d_draw", 2, 3, bi_r3d_draw);
  vm.define_native("r3d_pixels", 1, 1, bi_r3d_pixels);
  vm.define_native("r3d_stats", 1, 1, bi_r3d_stats);
}

}  // namespace aios::userland::grain
