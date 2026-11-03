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
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
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
  bool alpha = false;   // some texel is transparent
};

struct R3dTarget {
  int w = 0, h = 0;
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
//   r3d_draw_tex(t, rows, fov_deg[, camera])             -> how many triangles were filled; with camera
//       {x, y, z, angle} the rows are world points on Doom's map axes (x east, y north, z up), turned here
//       rows: maps with x0 y0 z0 u0 v0 .. x2 y2 z2 u2 v2 (camera space; u v in texels), tex (an id given to
//       r3d_texture), light (0..255) and flags: 1 masked (transparent texels let through what is behind), 2 full
//       bright (no light, no falloff), 4 clamp v (the texture is not repeated up or down: a masked middle texture),
//       8 fuzz (a spectre: what is behind darkened where the picture is opaque).
//   r3d_sky(t, tex, yaw_deg, fov_deg)                    -> t: the sky, Doom's way (the texture four times round the
//       horizon, its rows down from the top of the view), wherever nothing has been drawn: call it after r3d_draw_tex
//   r3d_blit(t, tex, x, y, w, h, light)                  -> t: the picture scaled to x, y, w, h on top of
//       everything (a weapon), transparent texels skipped
//   r3d_options(t, {bilinear, mipmaps, y_scale, light_gain, light_lift, light_fall}) -> t
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
  const uint32_t p00 = texel(L, x0, y0, wrap_v), p10 = texel(L, x0 + 1, y0, wrap_v);
  const uint32_t p01 = texel(L, x0, y0 + 1, wrap_v), p11 = texel(L, x0 + 1, y0 + 1, wrap_v);
  // weights of the four, and each channel weighted by its texel's alpha (so a transparent neighbour does not
  // bleed its black in): colour = sum(c a w) / sum(a w), alpha = sum(a w)
  const uint32_t w00 = (256 - ax) * (256 - ay), w10 = ax * (256 - ay), w01 = (256 - ax) * ay, w11 = ax * ay;
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

// One triangle in screen space with what the fill interpolates: 1/z, u/z and v/z are linear on the screen.
struct STri {
  int x0, x1, y0, y1;            // bounding box, clipped to the target
  double ea[3], eb[3], ec[3];    // edge functions, positive inside
  double qx, qy, qc;             // 1/z = qx x + qy y + qc
  double ux, uy, uc, vx, vy, vc; // u/z, v/z likewise
  const R3dTex* tex;
  float light;                   // 0..1
  int flags;
};

struct TV { double x, y, z, u, v; };

// The screen triangle of three projected points (sx, sy, 1/z, u/z, v/z); false when it covers nothing.
bool setup(const R3dTarget& t, const double* sx, const double* sy, const double* q, const double* uq, const double* vq, STri* o) {
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
  return true;
}

// Fill rows [ya, yb) of one triangle.
void fill_tex_rows(R3dTarget& t, const STri& s, int ya, int yb, int64_t* pixels) {
  const R3dTex* tx = s.tex;
  const bool masked = (s.flags & 1) != 0, bright = (s.flags & 2) != 0, wrap_v = (s.flags & 4) == 0, fuzz = (s.flags & 8) != 0;
  const int levels = (int)tx->mips.size();
  const double W0 = tx->mips[0].w, H0 = tx->mips[0].h;
  const float L = s.light;
  const double gain = t.light_gain, lift = t.light_lift, fall = t.light_fall;
  const bool bil = t.bilinear, mips = t.mipmaps;
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
        const double rho2 = std::sqrt((dux * dux + dvx * dvx) * (duy * duy + dvy * dvy));
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
        b = std::min(1.0, std::max(0.0, 1.0 - cmap / 32.0 * gain + lift));
      }
      const uint32_t r = (uint32_t)(((p >> 16) & 255) * b), g = (uint32_t)(((p >> 8) & 255) * b), bl = (uint32_t)((p & 255) * b);
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
        p[n++] = {v[i].x + (v[j].x - v[i].x) * k, v[i].y + (v[j].y - v[i].y) * k, kNear,
                  v[i].u + (v[j].u - v[i].u) * k, v[i].v + (v[j].v - v[i].v) * k};
      }
    }
  }
  double sx[4], sy[4], q[4], uq[4], vq[4];
  const double cx = t.w * 0.5, cy = t.h * 0.5, fy = focal * t.y_scale;
  for (int i = 0; i < n; ++i) {
    q[i] = 1.0 / p[i].z;
    sx[i] = cx + p[i].x * focal * q[i];
    sy[i] = cy - p[i].y * fy * q[i];
    uq[i] = p[i].u * q[i];
    vq[i] = p[i].v * q[i];
  }
  for (int k = 0; k + 2 < n; ++k) {   // (0 1 2), and for a quad (0 2 3)
    const int a = 0, b = k + 1, c = k + 2;
    const double tsx[3] = {sx[a], sx[b], sx[c]}, tsy[3] = {sy[a], sy[b], sy[c]}, tq[3] = {q[a], q[b], q[c]};
    const double tu[3] = {uq[a], uq[b], uq[c]}, tv[3] = {vq[a], vq[b], vq[c]};
    STri s;
    if (!setup(t, tsx, tsy, tq, tu, tv, &s)) continue;
    s.tex = tx;
    s.light = light;
    s.flags = flags;
    out->push_back(s);
  }
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
  t->tris = t->filled = t->clipped = t->pixels = 0;
  // With a camera {x, y, z, angle} (Doom's map axes: x east, y north, z up, angle in degrees from east), the rows
  // are in the world and are turned to the camera here, each frame, from geometry worked out once a tic.
  bool world = false;
  double cxw = 0, cyw = 0, czw = 0, ca = 1, sa = 0;
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
    world = true;
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
        v[i] = {dx * sa - dy * ca, v[i].z - czw, dx * ca + dy * sa, v[i].u, v[i].v};
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
  // The rows of the target in bands, one band per task: every triangle is filled band by band, so no two threads
  // write a pixel, and each pass (opaque, masked, fuzz) is finished before the next starts.
  const int band = 16;
  const size_t nb = (size_t)((t->h + band - 1) / band);
  std::vector<int64_t> px(SqlThreadPool::instance().num_workers() + 1, 0);
  for (int pass = 0; pass < 3; ++pass) {
    const std::vector<STri>& list = tris[pass];
    if (list.empty()) continue;
    t->filled += (int64_t)list.size();
    auto run = [&](size_t b0, size_t b1, size_t tid) {
      for (size_t b = b0; b < b1; ++b) {
        const int ya = (int)b * band, yb = std::min(t->h, ya + band);
        for (const STri& s : list)
          if (s.y1 >= ya && s.y0 < yb) fill_tex_rows(*t, s, ya, yb, &px[std::min(tid, px.size() - 1)]);
      }
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
  // Doom's: 1024 sky columns round the horizon (a 256-wide texture four times), each screen row the texture row
  // it would be on Doom's 200-row view (the horizon at row 100), the rows below the texture its last one.
  std::vector<double> col((size_t)t->w);
  for (int x = 0; x < t->w; ++x) {
    const double ang = -yaw + std::atan2(x + 0.5 - t->w * 0.5, focal) * 57.29577951308232;
    col[(size_t)x] = ang / 360.0 * 1024.0 * L.w / 256.0;
  }
  // Only the pixels nothing has been drawn on (depth 0): drawn after the scene, the sky costs what shows of it,
  // not the whole screen (6 ms a frame at 1280x960 when it went first), in bands across the pool as the fill is.
  const double rows_per_px = 200.0 / t->h;
  const bool bil = t->bilinear;
  R3dTarget& T = *t;
  auto rows = [&](size_t y0, size_t y1, size_t) {
    for (size_t y = y0; y < y1; ++y) {
      const double v = std::min((double)L.h - 0.51, (y + 0.5) * rows_per_px);
      uint32_t* crow = &T.color[y * T.w];
      const float* drow = &T.depth[y * T.w];
      for (int x = 0; x < T.w; ++x)
        if (drow[x] == 0.0f) crow[x] = sample(L, col[(size_t)x], v, false, bil) & 0xFFFFFF;
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
    } else if (k == "y_scale" || k == "light_gain" || k == "light_lift" || k == "light_fall") {
      if (!v.is_number()) { error = "r3d_options(): " + k + " wants a number"; return Value::nil(); }
      const double d = v.as_double();
      if (k == "y_scale") { if (!(d > 0.1 && d < 10)) { error = "r3d_options(): y_scale out of range"; return Value::nil(); } t->y_scale = d; }
      else if (k == "light_gain") t->light_gain = d;
      else if (k == "light_lift") t->light_lift = d;
      else t->light_fall = d;
    } else {
      error = "r3d_options(): no option " + k + " (bilinear, mipmaps, y_scale, light_gain, light_lift, light_fall)";
      return Value::nil();
    }
  }
  return args[0];
}

}  // namespace

void install_raster3d_builtins(Vm& vm) {
  vm.define_native("r3d_texture", 6, 7, bi_r3d_texture);
  vm.define_native("r3d_draw_tex", 2, 4, bi_r3d_draw_tex);
  vm.define_native("r3d_sky", 4, 4, bi_r3d_sky);
  vm.define_native("r3d_blit", 7, 7, bi_r3d_blit);
  vm.define_native("r3d_options", 2, 2, bi_r3d_options);
  vm.define_native("r3d_clear_depth", 1, 1, bi_r3d_clear_depth);
  vm.define_native("r3d_target", 2, 2, bi_r3d_target);
  vm.define_native("r3d_clear", 1, 2, bi_r3d_clear);
  vm.define_native("r3d_draw", 2, 3, bi_r3d_draw);
  vm.define_native("r3d_pixels", 1, 1, bi_r3d_pixels);
  vm.define_native("r3d_stats", 1, 1, bi_r3d_stats);
}

}  // namespace aios::userland::grain
