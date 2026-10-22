// The composer and the wire format for a scene the machine made up.
//
// No <sstream> and no exceptions: this file is in the bare-metal CORE glob, so
// it gets snprintf and strtod from metal/runtime.cpp and nothing else.
#include "aios/userland/scene_description.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace aios::userland {
namespace {

void set3(float* dst, float a, float b, float c) { dst[0] = a; dst[1] = b; dst[2] = c; }

std::string lower(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) o += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  return o;
}

bool mentions(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

// The request decides the scene, so the same words must give the same picture.
// A clock or a counter here would mean "show me a forest" drew somewhere else
// every time, which reads as the machine not having understood.
uint64_t seed_of(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
  return h ? h : 1ull;
}

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed) {}
  uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
  // [0,1)
  float unit() { return (float)((next() >> 11) & 0xFFFFFFull) / 16777216.0f; }
  float range(float a, float b) { return a + (b - a) * unit(); }
};

std::string fmt_f(float v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.4g", (double)v);
  return std::string(buf);
}

std::string fmt3(const float* v) {
  return fmt_f(v[0]) + " " + fmt_f(v[1]) + " " + fmt_f(v[2]);
}

// One line's worth of whitespace-separated fields.
std::vector<std::string> fields(const std::string& line) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
    size_t b = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') ++i;
    if (i > b) out.push_back(line.substr(b, i - b));
  }
  return out;
}

float to_f(const std::string& s) { return (float)std::strtod(s.c_str(), nullptr); }

}  // namespace

const char* solid_kind_name(SolidKind k) {
  switch (k) {
    case SolidKind::Sphere:     return "sphere";
    case SolidKind::Box:        return "box";
    case SolidKind::Pyramid:    return "pyramid";
    case SolidKind::Torus:      return "torus";
    case SolidKind::Cylinder:   return "cylinder";
    case SolidKind::Cone:       return "cone";
    case SolidKind::Capsule:    return "capsule";
    case SolidKind::Taper:      return "taper";
    case SolidKind::Sail:       return "sail";
    case SolidKind::HexPrism:   return "hex_prism";
    case SolidKind::Ellipsoid:  return "ellipsoid";
    case SolidKind::Ring:       return "ring";
    case SolidKind::Octahedron: return "octahedron";
    case SolidKind::Arch:       return "arch";
    case SolidKind::Star:       return "star";
  }
  return "sphere";
}

bool parse_solid_kind(const std::string& s, SolidKind& out) {
  if (s == "cone")                    { out = SolidKind::Cone;       return true; }
  if (s == "sphere")                  { out = SolidKind::Sphere;     return true; }
  if (s == "box")                     { out = SolidKind::Box;        return true; }
  if (s == "pyramid")                 { out = SolidKind::Pyramid;    return true; }
  if (s == "torus")                   { out = SolidKind::Torus;      return true; }
  if (s == "cylinder")                { out = SolidKind::Cylinder;   return true; }
  if (s == "capsule")                 { out = SolidKind::Capsule;    return true; }
  if (s == "taper")                   { out = SolidKind::Taper;      return true; }
  if (s == "sail")                    { out = SolidKind::Sail;       return true; }
  if (s == "hex_prism" || s == "prism" || s == "hexprism") { out = SolidKind::HexPrism; return true; }
  if (s == "ellipsoid" || s == "oval" || s == "egg")       { out = SolidKind::Ellipsoid; return true; }
  if (s == "ring" || s == "halo" || s == "disc")           { out = SolidKind::Ring; return true; }
  if (s == "octahedron" || s == "gem" || s == "diamond")   { out = SolidKind::Octahedron; return true; }
  if (s == "arch" || s == "portal" || s == "gate")         { out = SolidKind::Arch; return true; }
  if (s == "star" || s == "spike")                         { out = SolidKind::Star; return true; }
  return false;
}

const char* ground_kind_name(GroundKind k) {
  switch (k) {
    case GroundKind::None:    return "none";
    case GroundKind::Plain:   return "plain";
    case GroundKind::Water:   return "water";
    case GroundKind::Hills:   return "hills";
    case GroundKind::Dunes:   return "dunes";
    case GroundKind::River:   return "river";
    case GroundKind::Canyon:  return "canyon";
    case GroundKind::Volcano: return "volcano";
    case GroundKind::Glacier: return "glacier";
    case GroundKind::Ocean:   return "ocean";
  }
  return "plain";
}

bool parse_ground_kind(const std::string& s, GroundKind& out) {
  if (s == "none")                           { out = GroundKind::None;    return true; }
  if (s == "plain")                          { out = GroundKind::Plain;   return true; }
  if (s == "water")                          { out = GroundKind::Water;   return true; }
  if (s == "hills")                          { out = GroundKind::Hills;   return true; }
  if (s == "dunes")                          { out = GroundKind::Dunes;   return true; }
  if (s == "river")                          { out = GroundKind::River;   return true; }
  if (s == "canyon" || s == "gorge" || s == "mesa") { out = GroundKind::Canyon; return true; }
  if (s == "volcano" || s == "caldera" || s == "lava") { out = GroundKind::Volcano; return true; }
  if (s == "glacier" || s == "ice")          { out = GroundKind::Glacier; return true; }
  if (s == "ocean" || s == "sea")            { out = GroundKind::Ocean;   return true; }
  return false;
}

std::vector<float> SceneDescription::to_floats() const {
  std::vector<float> v;
  v.reserve(kHeaderVec4s * 4 + solids.size() * kSolidVec4s * 4);
  auto push4 = [&](float a, float b, float c, float d) {
    v.push_back(a); v.push_back(b); v.push_back(c); v.push_back(d);
  };
  // Normalised here rather than in the shader: it is once per upload instead
  // of once per pixel, and a description written by hand with a lazy direction
  // still lights correctly.
  float sl = std::sqrt(sun_dir[0] * sun_dir[0] + sun_dir[1] * sun_dir[1] + sun_dir[2] * sun_dir[2]);
  if (sl < 1e-6f) sl = 1.0f;

  push4(sky_low[0], sky_low[1], sky_low[2], 0.0f);
  push4(sky_high[0], sky_high[1], sky_high[2], 0.0f);
  push4(sun_dir[0] / sl, sun_dir[1] / sl, sun_dir[2] / sl, sun_sharpness);
  push4(sun_colour[0], sun_colour[1], sun_colour[2], sun_intensity);
  push4(ground_colour[0], ground_colour[1], ground_colour[2], (float)(uint32_t)ground);
  push4(fog_colour[0], fog_colour[1], fog_colour[2], fog_density);
  push4(camera_radius, camera_height, camera_fov, camera_spin);
  push4((float)solids.size(), ambient, 0.0f, 0.0f);
  // The bounding sphere around everything standing in the scene. Computed here
  // because it is once per upload rather than once per march step.
  {
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    if (!solids.empty()) {
      for (const auto& s : solids) { cx += s.x; cy += s.y; cz += s.z; }
      const float inv = 1.0f / (float)solids.size();
      cx *= inv; cy *= inv; cz *= inv;
    }
    float rad = 0.0f;
    for (const auto& s : solids) {
      const float bulk = std::max(std::max(s.sx, s.sy), std::max(s.sz, std::max(s.r0, s.r1)));
      // Both ends, for the kinds that are a segment or a triangle.
      const float pts[3][3] = {{s.x, s.y, s.z}, {s.x1, s.y1, s.z1}, {s.x2, s.y2, s.z2}};
      const int npts = (s.kind == SolidKind::Capsule || s.kind == SolidKind::Taper) ? 2
                     : (s.kind == SolidKind::Sail ? 3 : 1);
      for (int i = 0; i < npts; ++i) {
        const float dx = pts[i][0] - cx, dy = pts[i][1] - cy, dz = pts[i][2] - cz;
        rad = std::max(rad, std::sqrt(dx * dx + dy * dy + dz * dz) + bulk + s.blend);
      }
    }
    push4(cx, cy, cz, solids.empty() ? 0.0f : rad * 1.05f);
  }
  for (const auto& s : solids) {
    push4((float)(uint32_t)s.kind, s.x, s.y, s.z);
    push4(s.sx, s.sy, s.sz, s.blend);
    push4(s.r, s.g, s.b, s.gloss);
    push4(s.x1, s.y1, s.z1, s.r0);
    push4(s.x2, s.y2, s.z2, s.r1);
  }
  return v;
}

std::string SceneDescription::to_text() const {
  std::string t;
  t += "# a scene this machine composed\n";
  t += "summary " + summary + "\n";
  t += "sky_low " + fmt3(sky_low) + "\n";
  t += "sky_high " + fmt3(sky_high) + "\n";
  t += "sun_dir " + fmt3(sun_dir) + "\n";
  t += "sun_colour " + fmt3(sun_colour) + "\n";
  t += "sun " + fmt_f(sun_intensity) + " " + fmt_f(sun_sharpness) + "\n";
  t += "ground " + std::string(ground_kind_name(ground)) + " " + fmt3(ground_colour) + "\n";
  t += "fog " + fmt3(fog_colour) + " " + fmt_f(fog_density) + "\n";
  t += "ambient " + fmt_f(ambient) + "\n";
  t += "camera " + fmt_f(camera_radius) + " " + fmt_f(camera_height) + " " +
       fmt_f(camera_fov) + " " + fmt_f(camera_spin) + "\n";
  for (const auto& s : solids) {
    t += "solid ";
    t += solid_kind_name(s.kind);
    t += " at " + fmt_f(s.x) + " " + fmt_f(s.y) + " " + fmt_f(s.z);
    t += " size " + fmt_f(s.sx) + " " + fmt_f(s.sy) + " " + fmt_f(s.sz);
    // The second and third points, and the radii, only for the kinds that read
    // them: a sphere line with a "to" on it would be a line about nothing.
    if (s.kind == SolidKind::Capsule || s.kind == SolidKind::Taper ||
        s.kind == SolidKind::Sail) {
      t += " to " + fmt_f(s.x1) + " " + fmt_f(s.y1) + " " + fmt_f(s.z1);
    }
    if (s.kind == SolidKind::Sail) {
      t += " and " + fmt_f(s.x2) + " " + fmt_f(s.y2) + " " + fmt_f(s.z2);
      t += " thick " + fmt_f(s.r0);
    } else if (s.kind == SolidKind::Capsule) {
      t += " radius " + fmt_f(s.r0);
    } else if (s.kind == SolidKind::Taper) {
      t += " radii " + fmt_f(s.r0) + " " + fmt_f(s.r1);
    }
    t += " colour " + fmt_f(s.r) + " " + fmt_f(s.g) + " " + fmt_f(s.b);
    t += " gloss " + fmt_f(s.gloss);
    if (s.blend > 0.0f) t += " blend " + fmt_f(s.blend);
    t += "\n";
  }
  return t;
}

Result<SceneDescription> SceneDescription::from_text(const std::string& text) {
  SceneDescription d;
  d.solids.clear();
  size_t at = 0;
  bool saw_any = false;
  while (at <= text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    at = end + 1;
    if (line.empty() || line[0] == '#') { if (at > text.size()) break; continue; }
    auto f = fields(line);
    if (f.empty()) { if (at > text.size()) break; continue; }
    const std::string& key = f[0];
    if (key == "summary") {
      size_t sp = line.find(' ');
      d.summary = (sp == std::string::npos) ? std::string() : line.substr(sp + 1);
      saw_any = true;
    } else if (key == "sky_low" && f.size() >= 4) {
      set3(d.sky_low, to_f(f[1]), to_f(f[2]), to_f(f[3])); saw_any = true;
    } else if (key == "sky_high" && f.size() >= 4) {
      set3(d.sky_high, to_f(f[1]), to_f(f[2]), to_f(f[3])); saw_any = true;
    } else if (key == "sun_dir" && f.size() >= 4) {
      set3(d.sun_dir, to_f(f[1]), to_f(f[2]), to_f(f[3])); saw_any = true;
    } else if (key == "sun_colour" && f.size() >= 4) {
      set3(d.sun_colour, to_f(f[1]), to_f(f[2]), to_f(f[3])); saw_any = true;
    } else if (key == "sun" && f.size() >= 3) {
      d.sun_intensity = to_f(f[1]); d.sun_sharpness = to_f(f[2]); saw_any = true;
    } else if (key == "ground" && f.size() >= 5) {
      if (!parse_ground_kind(f[1], d.ground))
        return Result<SceneDescription>(Status::Invalid, "unknown ground '" + f[1] + "'");
      set3(d.ground_colour, to_f(f[2]), to_f(f[3]), to_f(f[4])); saw_any = true;
    } else if (key == "fog" && f.size() >= 5) {
      set3(d.fog_colour, to_f(f[1]), to_f(f[2]), to_f(f[3]));
      d.fog_density = to_f(f[4]); saw_any = true;
    } else if (key == "ambient" && f.size() >= 2) {
      d.ambient = to_f(f[1]); saw_any = true;
    } else if (key == "camera" && f.size() >= 5) {
      d.camera_radius = to_f(f[1]); d.camera_height = to_f(f[2]);
      d.camera_fov = to_f(f[3]);   d.camera_spin = to_f(f[4]); saw_any = true;
    } else if (key == "solid" && f.size() >= 2) {
      Solid s;
      if (!parse_solid_kind(f[1], s.kind))
        return Result<SceneDescription>(Status::Invalid, "unknown solid '" + f[1] + "'");
      // Keyword-led rather than positional, so a hand-written line can leave
      // out what it does not care about and still mean something.
      for (size_t i = 2; i < f.size();) {
        if (f[i] == "at" && i + 3 < f.size()) {
          s.x = to_f(f[i + 1]); s.y = to_f(f[i + 2]); s.z = to_f(f[i + 3]); i += 4;
        } else if (f[i] == "size" && i + 3 < f.size()) {
          s.sx = to_f(f[i + 1]); s.sy = to_f(f[i + 2]); s.sz = to_f(f[i + 3]); i += 4;
        } else if (f[i] == "colour" && i + 3 < f.size()) {
          s.r = to_f(f[i + 1]); s.g = to_f(f[i + 2]); s.b = to_f(f[i + 3]); i += 4;
        } else if (f[i] == "gloss" && i + 1 < f.size()) {
          s.gloss = to_f(f[i + 1]); i += 2;
        } else if (f[i] == "to" && i + 3 < f.size()) {
          s.x1 = to_f(f[i + 1]); s.y1 = to_f(f[i + 2]); s.z1 = to_f(f[i + 3]); i += 4;
        } else if (f[i] == "and" && i + 3 < f.size()) {
          s.x2 = to_f(f[i + 1]); s.y2 = to_f(f[i + 2]); s.z2 = to_f(f[i + 3]); i += 4;
        } else if ((f[i] == "radius" || f[i] == "thick") && i + 1 < f.size()) {
          s.r0 = to_f(f[i + 1]); i += 2;
        } else if (f[i] == "radii" && i + 2 < f.size()) {
          s.r0 = to_f(f[i + 1]); s.r1 = to_f(f[i + 2]); i += 3;
        } else if (f[i] == "blend" && i + 1 < f.size()) {
          s.blend = to_f(f[i + 1]); i += 2;
        } else {
          ++i;
        }
      }
      d.solids.push_back(s);
      saw_any = true;
    }
    if (at > text.size()) break;
  }
  if (!saw_any) return Result<SceneDescription>(Status::Invalid, "no scene in this text");
  return d;
}

// ---------------------------------------------------------------------------
// The composer.
//
// A vocabulary, not a parser: the request is scanned for things this machine
// knows how to put in a picture, and whatever it finds it places. The contract
// is that there is NO request it refuses — an unrecognised one still gets a
// scene, described honestly as having been made from those words, because
// "this machine does not know how to draw that" was the whole complaint.
// ---------------------------------------------------------------------------
namespace {

struct Palette { const char* word; float r, g, b; };
const Palette kColours[] = {
  {"crimson", 0.72f, 0.10f, 0.14f}, {"scarlet", 0.80f, 0.16f, 0.10f},
  {"red",     0.68f, 0.16f, 0.16f}, {"orange",  0.85f, 0.45f, 0.12f},
  {"amber",   0.85f, 0.60f, 0.15f}, {"gold",    0.83f, 0.68f, 0.22f},
  {"yellow",  0.85f, 0.80f, 0.25f}, {"emerald", 0.12f, 0.60f, 0.36f},
  {"green",   0.24f, 0.55f, 0.28f}, {"teal",    0.12f, 0.52f, 0.52f},
  {"cyan",    0.25f, 0.72f, 0.80f}, {"azure",   0.20f, 0.48f, 0.82f},
  {"blue",    0.20f, 0.34f, 0.72f}, {"violet",  0.42f, 0.24f, 0.72f},
  {"purple",  0.44f, 0.20f, 0.62f}, {"magenta", 0.72f, 0.20f, 0.60f},
  {"pink",    0.85f, 0.48f, 0.60f}, {"white",   0.88f, 0.88f, 0.90f},
  {"silver",  0.72f, 0.74f, 0.78f}, {"grey",    0.48f, 0.48f, 0.50f},
  {"gray",    0.48f, 0.48f, 0.50f}, {"black",   0.08f, 0.08f, 0.09f},
  {"bronze",  0.60f, 0.40f, 0.20f}, {"copper",  0.66f, 0.36f, 0.20f},
  {"sand",    0.76f, 0.66f, 0.46f},
};

struct CountWord { const char* word; int n; };
const CountWord kCounts[] = {
  {"twelve", 12}, {"eleven", 11}, {"ten", 10}, {"nine", 9}, {"eight", 8},
  {"seven", 7}, {"six", 6}, {"five", 5}, {"four", 4}, {"three", 3}, {"two", 2},
  {"a pair of", 2}, {"a couple", 2}, {"a few", 3}, {"several", 5},
  {"many", 9}, {"lots of", 9}, {"a forest of", 9}, {"one", 1}, {"a single", 1},
};

// Scatter n solids so they do not intersect and the camera can see past them.
// A ring reads as deliberate placement; pure random overlapped and looked like
// a mistake rather than a composition.
void place_ring(Rng& rng, std::vector<Solid>& out, Solid proto, int n, float radius) {
  if (n <= 1) { proto.x = 0.0f; proto.z = 0.0f; out.push_back(proto); return; }
  const float turn = 6.2831853f / (float)n;
  const float phase = rng.unit() * 6.2831853f;
  for (int i = 0; i < n; ++i) {
    Solid s = proto;
    const float a = phase + turn * (float)i;
    const float rr = radius * rng.range(0.78f, 1.18f);
    s.x = std::cos(a) * rr;
    s.z = std::sin(a) * rr;
    const float scale = rng.range(0.72f, 1.30f);
    s.sx *= scale; s.sy *= scale; s.sz *= scale;
    // Sit it ON the ground rather than through it: the y in the prototype is
    // the half-height, so scaling it has to move the centre too.
    if (proto.kind == SolidKind::Sphere)        s.y = s.sx;
    else if (proto.kind == SolidKind::Box)      s.y = s.sy;
    else if (proto.kind == SolidKind::Cylinder) s.y = s.sy;
    else if (proto.kind == SolidKind::Pyramid)  s.y = 0.0f;
    else if (proto.kind == SolidKind::Cone)     s.y = 0.0f;
    else                                         s.y = proto.y * scale;
    out.push_back(s);
  }
}

int count_in(const std::string& q, bool plural_hint) {
  for (const auto& c : kCounts) if (mentions(q, c.word)) return c.n;
  return plural_hint ? 3 : 1;
}

bool colour_in(const std::string& q, float* rgb) {
  for (const auto& c : kColours) {
    if (mentions(q, c.word)) { set3(rgb, c.r, c.g, c.b); return true; }
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// The dragon
// ---------------------------------------------------------------------------
// The first thing this machine composes that is a CREATURE rather than an
// arrangement of objects, and the reason the segment primitives exist: every
// part of it points somewhere, and none of those directions is an axis.
//
// It is laid out in its own frame — nose toward +x, up is +y, wings across z —
// as a skeleton of points with flesh hung between them. Each limb is a taper
// from one joint to the next, which means the pose is the DATA: moving a joint
// moves everything that hangs off it, and there is no rotation anywhere.
//
// Everything on the body carries a blend, so the field melds into one animal
// instead of a heap of cones that happen to touch. The wing membranes do not:
// a membrane is a sheet, and blending it into the arm that holds it would
// thicken exactly the edge that should stay sharp.
namespace {

struct Hide { float r, g, b, gloss; };

void add_taper(std::vector<Solid>& out, float ax, float ay, float az,
               float bx, float by, float bz, float ra, float rb,
               const Hide& hide, float blend) {
  Solid s;
  s.kind = SolidKind::Taper;
  s.x = ax; s.y = ay; s.z = az;
  s.x1 = bx; s.y1 = by; s.z1 = bz;
  s.r0 = ra; s.r1 = rb;
  s.r = hide.r; s.g = hide.g; s.b = hide.b; s.gloss = hide.gloss;
  s.blend = blend;
  out.push_back(s);
}

void add_sail(std::vector<Solid>& out, float ax, float ay, float az,
              float bx, float by, float bz, float cx, float cy, float cz,
              float thick, const Hide& hide) {
  Solid s;
  s.kind = SolidKind::Sail;
  s.x = ax; s.y = ay; s.z = az;
  s.x1 = bx; s.y1 = by; s.z1 = bz;
  s.x2 = cx; s.y2 = cy; s.z2 = cz;
  s.r0 = thick;
  s.r = hide.r; s.g = hide.g; s.b = hide.b; s.gloss = hide.gloss;
  s.blend = 0.0f;
  out.push_back(s);
}

// One wing, on whichever side `side` is (+1 / -1 in z).
//
// A bat's hand: an arm to the wrist, then THREE fingers fanning back from it,
// with membrane stretched between each pair and the last one running down to
// the hip. Two spars gave two big flat panels that read as a slab; the whole
// character of a wing is in the taper between its fingers.
void add_wing(std::vector<Solid>& out, float side, const Hide& hide, const Hide& web) {
  const float sx = 0.55f, sy = 2.30f, sz = 0.34f * side;      // shoulder
  const float wx = 0.30f, wy = 3.55f, wz = 1.75f * side;      // wrist, held high
  // Fingers fan back and down from the wrist.
  const float f1x = -0.35f, f1y = 3.60f, f1z = 3.25f * side;
  const float f2x = -1.30f, f2y = 2.95f, f2z = 3.05f * side;
  const float f3x = -1.95f, f3y = 2.05f, f3z = 2.20f * side;
  const float hipx = -1.30f, hipy = 1.60f, hipz = 0.50f * side;

  add_taper(out, sx, sy, sz, wx, wy, wz, 0.16f, 0.075f, hide, 0.12f);
  add_taper(out, wx, wy, wz, f1x, f1y, f1z, 0.075f, 0.028f, hide, 0.05f);
  add_taper(out, wx, wy, wz, f2x, f2y, f2z, 0.070f, 0.026f, hide, 0.05f);
  add_taper(out, wx, wy, wz, f3x, f3y, f3z, 0.065f, 0.024f, hide, 0.05f);
  // Membrane between each pair of spars, and the trailing edge down to the hip.
  add_sail(out, sx, sy, sz, wx, wy, wz, f1x, f1y, f1z, 0.028f, web);
  add_sail(out, wx, wy, wz, f1x, f1y, f1z, f2x, f2y, f2z, 0.028f, web);
  add_sail(out, wx, wy, wz, f2x, f2y, f2z, f3x, f3y, f3z, 0.028f, web);
  add_sail(out, sx, sy, sz, wx, wy, wz, f3x, f3y, f3z, 0.028f, web);
  add_sail(out, sx, sy, sz, f3x, f3y, f3z, hipx, hipy, hipz, 0.028f, web);
}

// One leg: thigh, shin, foot.
void add_leg(std::vector<Solid>& out, float hx, float side, float reach, const Hide& hide) {
  const float z = 0.52f * side;
  add_taper(out, hx, 1.45f, z, hx + reach * 0.25f, 0.85f, z * 1.18f, 0.21f, 0.15f, hide, 0.22f);
  add_taper(out, hx + reach * 0.25f, 0.85f, z * 1.18f, hx + reach, 0.16f, z * 1.25f, 0.15f, 0.085f, hide, 0.18f);
  add_taper(out, hx + reach, 0.16f, z * 1.25f, hx + reach + 0.26f, 0.07f, z * 1.25f, 0.095f, 0.05f, hide, 0.12f);
}

void compose_dragon(std::vector<Solid>& out, const Hide& hide, const Hide& web) {
  const float B = 0.30f;   // how far the body parts meld into each other

  // Spine: hips to chest, thickest at the ribs.
  add_taper(out, -1.35f, 1.52f, 0.0f, -0.45f, 1.62f, 0.0f, 0.40f, 0.48f, hide, B);
  add_taper(out, -0.45f, 1.62f, 0.0f,  0.35f, 1.70f, 0.0f, 0.48f, 0.50f, hide, B);
  add_taper(out,  0.35f, 1.70f, 0.0f,  1.00f, 1.74f, 0.0f, 0.50f, 0.38f, hide, B);

  // Neck: four segments and longer than it looks it should be. A short neck on
  // a heavy body reads as a horse with horns; the length is most of what makes
  // the silhouette a dragon.
  add_taper(out, 1.00f, 1.74f, 0.0f, 1.70f, 2.35f, 0.0f, 0.33f, 0.26f, hide, B);
  add_taper(out, 1.70f, 2.35f, 0.0f, 2.25f, 3.05f, 0.0f, 0.26f, 0.21f, hide, B);
  add_taper(out, 2.25f, 3.05f, 0.0f, 2.70f, 3.62f, 0.0f, 0.21f, 0.17f, hide, B);
  add_taper(out, 2.70f, 3.62f, 0.0f, 3.05f, 3.92f, 0.0f, 0.17f, 0.16f, hide, B);

  // Skull, then a narrower snout off it, then the jaw slung under: three parts
  // rather than one cone, which is what gives a head a brow and a mouth line.
  add_taper(out, 3.05f, 3.92f, 0.0f, 3.48f, 3.95f, 0.0f, 0.19f, 0.145f, hide, 0.16f);
  add_taper(out, 3.48f, 3.95f, 0.0f, 3.95f, 3.86f, 0.0f, 0.13f, 0.075f, hide, 0.10f);
  add_taper(out, 3.30f, 3.83f, 0.0f, 3.88f, 3.78f, 0.0f, 0.085f, 0.045f, hide, 0.07f);

  // Horns, swept back off the skull, and a pair of smaller ones beside them.
  for (int i = 0; i < 2; ++i) {
    const float side = i == 0 ? 1.0f : -1.0f;
    add_taper(out, 3.12f, 4.06f, 0.09f * side, 2.62f, 4.72f, 0.26f * side, 0.060f, 0.010f, hide, 0.04f);
    add_taper(out, 3.18f, 3.98f, 0.14f * side, 2.88f, 4.30f, 0.34f * side, 0.036f, 0.008f, hide, 0.03f);
  }

  // Tail: five segments, falling then lifting at the tip.
  add_taper(out, -1.35f, 1.52f, 0.0f, -2.10f, 1.34f, 0.0f, 0.34f, 0.26f, hide, B);
  add_taper(out, -2.10f, 1.34f, 0.0f, -2.90f, 1.12f, 0.0f, 0.26f, 0.18f, hide, B);
  add_taper(out, -2.90f, 1.12f, 0.0f, -3.60f, 1.02f, 0.0f, 0.18f, 0.12f, hide, 0.22f);
  add_taper(out, -3.60f, 1.02f, 0.0f, -4.20f, 1.18f, 0.0f, 0.12f, 0.07f, hide, 0.16f);
  add_taper(out, -4.20f, 1.18f, 0.0f, -4.62f, 1.52f, 0.0f, 0.07f, 0.02f, hide, 0.10f);

  add_leg(out,  0.62f,  1.0f,  0.18f, hide);
  add_leg(out,  0.62f, -1.0f,  0.18f, hide);
  add_leg(out, -0.95f,  1.0f, -0.16f, hide);
  add_leg(out, -0.95f, -1.0f, -0.16f, hide);

  add_wing(out,  1.0f, hide, web);
  add_wing(out, -1.0f, hide, web);
}

// Castle / Fortress
void compose_castle(std::vector<Solid>& out, float r, float g, float b) {
  // 4 Corner Towers with conical spires
  const float span = 2.4f;
  for (int i = 0; i < 4; ++i) {
    const float tx = (i % 2 == 0 ? 1.0f : -1.0f) * span;
    const float tz = (i / 2 == 0 ? 1.0f : -1.0f) * span;
    // Tower cylinder
    Solid cyl; cyl.kind = SolidKind::Cylinder;
    cyl.x = tx; cyl.y = 1.8f; cyl.z = tz;
    cyl.sx = 0.55f; cyl.sy = 1.8f; cyl.sz = 0.55f;
    cyl.r = r; cyl.g = g; cyl.b = b; cyl.gloss = 0.12f;
    out.push_back(cyl);
    // Spire cone
    Solid spire; spire.kind = SolidKind::Cone;
    spire.x = tx; spire.y = 3.6f; spire.z = tz;
    spire.sx = 0.65f; spire.sy = 1.4f; spire.sz = 0.65f;
    spire.r = std::max(0.1f, r * 0.7f); spire.g = std::max(0.1f, g * 0.7f); spire.b = std::min(1.0f, b * 1.3f);
    spire.gloss = 0.25f;
    out.push_back(spire);
  }
  // 4 Curtain Walls
  for (int i = 0; i < 4; ++i) {
    Solid wall; wall.kind = SolidKind::Box;
    if (i < 2) {
      wall.x = (i == 0 ? 1.0f : -1.0f) * span; wall.y = 1.2f; wall.z = 0.0f;
      wall.sx = 0.25f; wall.sy = 1.2f; wall.sz = span - 0.4f;
    } else {
      wall.x = 0.0f; wall.y = 1.2f; wall.z = (i == 2 ? 1.0f : -1.0f) * span;
      wall.sx = span - 0.4f; wall.sy = 1.2f; wall.sz = 0.25f;
    }
    wall.r = r * 0.95f; wall.g = g * 0.95f; wall.b = b * 0.95f; wall.gloss = 0.10f;
    out.push_back(wall);
  }
  // Central Great Keep
  Solid keep; keep.kind = SolidKind::Box;
  keep.x = 0.0f; keep.y = 2.2f; keep.z = 0.0f;
  keep.sx = 1.1f; keep.sy = 2.2f; keep.sz = 1.1f;
  keep.r = r * 1.05f; keep.g = g * 1.05f; keep.b = b * 1.05f; keep.gloss = 0.15f;
  out.push_back(keep);
  // Keep Pyramid Roof
  Solid roof; roof.kind = SolidKind::Pyramid;
  roof.x = 0.0f; roof.y = 4.4f; roof.z = 0.0f;
  roof.sx = 1.25f; roof.sy = 1.1f; roof.sz = 1.25f;
  roof.r = 0.65f; roof.g = 0.22f; roof.b = 0.18f; roof.gloss = 0.20f;
  out.push_back(roof);
  // Gate Arch
  Solid arch; arch.kind = SolidKind::Arch;
  arch.x = 0.0f; arch.y = 0.0f; arch.z = span + 0.15f;
  arch.sx = 0.65f; arch.sy = 0.40f; arch.sz = 0.35f;
  arch.r = r * 0.85f; arch.g = g * 0.85f; arch.b = b * 0.85f; arch.gloss = 0.15f;
  out.push_back(arch);
}

// Spaceship / Starfighter
void compose_spaceship(std::vector<Solid>& out, float r, float g, float b) {
  // Fuselage body (ellipsoid)
  Solid body; body.kind = SolidKind::Ellipsoid;
  body.x = 0.0f; body.y = 1.8f; body.z = 0.0f;
  body.sx = 2.6f; body.sy = 0.45f; body.sz = 0.85f;
  body.r = r; body.g = g; body.b = b; body.gloss = 0.65f;
  out.push_back(body);
  // Cockpit Glass Canopy (cyan gloss)
  Solid glass; glass.kind = SolidKind::Ellipsoid;
  glass.x = 0.85f; glass.y = 2.15f; glass.z = 0.0f;
  glass.sx = 0.95f; glass.sy = 0.32f; glass.sz = 0.42f;
  glass.r = 0.15f; glass.g = 0.75f; glass.b = 0.95f; glass.gloss = 0.95f;
  out.push_back(glass);
  // Swept Wings (sails)
  for (int i = 0; i < 2; ++i) {
    const float side = i == 0 ? 1.0f : -1.0f;
    Solid wing; wing.kind = SolidKind::Sail;
    wing.x = 0.4f; wing.y = 1.75f; wing.z = 0.7f * side;
    wing.x1 = -1.8f; wing.y1 = 1.75f; wing.z1 = 2.8f * side;
    wing.x2 = -1.9f; wing.y2 = 1.75f; wing.z2 = 0.6f * side;
    wing.r0 = 0.06f;
    wing.r = r * 0.85f; wing.g = g * 0.85f; wing.b = b * 0.85f; wing.gloss = 0.55f;
    out.push_back(wing);
    // Thruster Engine Nacelles
    Solid eng; eng.kind = SolidKind::Cylinder;
    eng.x = -1.6f; eng.y = 1.8f; eng.z = 1.1f * side;
    eng.sx = 0.28f; eng.sy = 0.65f; eng.sz = 0.28f;
    eng.r = 0.25f; eng.g = 0.27f; eng.b = 0.32f; eng.gloss = 0.75f;
    out.push_back(eng);
    // Exhaust Plasma Ring (glowing orange/cyan)
    Solid ring; ring.kind = SolidKind::Ring;
    ring.x = -2.28f; ring.y = 1.8f; ring.z = 1.1f * side;
    ring.sx = 0.26f; ring.sy = 0.08f; ring.sz = 0.08f;
    ring.r = 0.2f; ring.g = 0.8f; ring.b = 1.0f; ring.gloss = 0.95f;
    out.push_back(ring);
  }
}

// Mech / Robot Golem
void compose_mech(std::vector<Solid>& out, float r, float g, float b) {
  // Torso Chassis (Hexagonal Prism)
  Solid torso; torso.kind = SolidKind::HexPrism;
  torso.x = 0.0f; torso.y = 2.4f; torso.z = 0.0f;
  torso.sx = 0.75f; torso.sy = 0.70f; torso.sz = 0.75f;
  torso.r = r; torso.g = g; torso.b = b; torso.gloss = 0.45f;
  out.push_back(torso);
  // Head Visor (Box)
  Solid head; head.kind = SolidKind::Box;
  head.x = 0.0f; head.y = 3.35f; head.z = 0.0f;
  head.sx = 0.35f; head.sy = 0.25f; head.sz = 0.35f;
  head.r = 0.22f; head.g = 0.24f; head.b = 0.28f; head.gloss = 0.60f;
  out.push_back(head);
  // Glowing Visor Eye
  Solid visor; visor.kind = SolidKind::Box;
  visor.x = 0.32f; visor.y = 3.35f; visor.z = 0.0f;
  visor.sx = 0.06f; visor.sy = 0.08f; visor.sz = 0.24f;
  visor.r = 0.95f; visor.g = 0.25f; visor.b = 0.15f; visor.gloss = 0.95f;
  out.push_back(visor);
  // Shoulders & Arms
  for (int i = 0; i < 2; ++i) {
    const float side = i == 0 ? 1.0f : -1.0f;
    // Shoulder Sphere
    Solid sh; sh.kind = SolidKind::Sphere;
    sh.x = 0.0f; sh.y = 2.85f; sh.z = 1.05f * side;
    sh.sx = 0.32f; sh.sy = 0.32f; sh.sz = 0.32f;
    sh.r = 0.30f; sh.g = 0.32f; sh.b = 0.36f; sh.gloss = 0.50f;
    out.push_back(sh);
    // Upper arm & Blaster forearm
    Solid arm; arm.kind = SolidKind::Taper;
    arm.x = 0.0f; arm.y = 2.85f; arm.z = 1.05f * side;
    arm.x1 = 0.35f; arm.y1 = 1.60f; arm.z1 = 1.15f * side;
    arm.r0 = 0.20f; arm.r1 = 0.14f;
    arm.r = r; arm.g = g; arm.b = b; arm.gloss = 0.45f;
    out.push_back(arm);
    // Leg: Hip to foot
    Solid leg; leg.kind = SolidKind::Taper;
    leg.x = 0.0f; leg.y = 1.70f; leg.z = 0.55f * side;
    leg.x1 = 0.15f; leg.y1 = 0.15f; leg.z1 = 0.65f * side;
    leg.r0 = 0.24f; leg.r1 = 0.16f;
    leg.r = r * 0.9f; leg.g = g * 0.9f; leg.b = b * 0.9f; leg.gloss = 0.40f;
    out.push_back(leg);
  }
}

// Crystals / Geode Gemstones
void compose_crystals(std::vector<Solid>& out, float r, float g, float b) {
  const int n = 9;
  for (int i = 0; i < n; ++i) {
    const float a = (float)i * 6.2831853f / (float)n + 0.3f;
    const float rad = (i == 0) ? 0.0f : 0.85f + 0.3f * (float)(i % 3);
    const float h = (i == 0) ? 2.6f : 1.2f + 0.4f * (float)((i * 3) % 5);
    Solid cr;
    if (i % 2 == 0) {
      cr.kind = SolidKind::HexPrism;
      cr.sx = 0.26f; cr.sy = h * 0.5f; cr.sz = 0.26f;
      cr.x = std::cos(a) * rad; cr.y = h * 0.5f; cr.z = std::sin(a) * rad;
    } else {
      cr.kind = SolidKind::Octahedron;
      cr.sx = 0.45f; cr.sy = 0.45f; cr.sz = 0.45f;
      cr.x = std::cos(a) * rad; cr.y = 0.45f + 0.2f * (float)i; cr.z = std::sin(a) * rad;
    }
    cr.r = r; cr.g = g; cr.b = b; cr.gloss = 0.92f;
    out.push_back(cr);
  }
}

// Pagoda / Shrine
void compose_pagoda(std::vector<Solid>& out, float r, float g, float b) {
  const int tiers = 3;
  for (int t = 0; t < tiers; ++t) {
    const float y_base = (float)t * 1.35f;
    const float scale = 1.0f - (float)t * 0.22f;
    // Pillar Tier Body
    Solid body; body.kind = SolidKind::Box;
    body.x = 0.0f; body.y = y_base + 0.45f; body.z = 0.0f;
    body.sx = 0.95f * scale; body.sy = 0.45f; body.sz = 0.95f * scale;
    body.r = 0.55f; body.g = 0.22f; body.b = 0.16f; body.gloss = 0.20f;
    out.push_back(body);
    // Flared Roof (Pyramid)
    Solid roof; roof.kind = SolidKind::Pyramid;
    roof.x = 0.0f; roof.y = y_base + 0.90f; roof.z = 0.0f;
    roof.sx = 1.65f * scale; roof.sy = 0.45f; roof.sz = 1.65f * scale;
    roof.r = 0.18f; roof.g = 0.20f; roof.b = 0.24f; roof.gloss = 0.45f;
    out.push_back(roof);
  }
  // Finial Spire
  Solid spire; spire.kind = SolidKind::Cone;
  spire.x = 0.0f; spire.y = 4.05f; spire.z = 0.0f;
  spire.sx = 0.18f; spire.sy = 1.1f; spire.sz = 0.18f;
  spire.r = 0.85f; spire.g = 0.72f; spire.b = 0.25f; spire.gloss = 0.85f;
  out.push_back(spire);
}

// Palm Trees
void compose_palm_trees(std::vector<Solid>& out, int n) {
  for (int i = 0; i < n; ++i) {
    const float a = (float)i * 6.2831853f / (float)std::max(1, n);
    const float dist = n > 1 ? 2.8f : 0.0f;
    const float px = std::cos(a) * dist, pz = std::sin(a) * dist;
    // Curved trunk tapers
    Solid t1; t1.kind = SolidKind::Taper;
    t1.x = px; t1.y = 0.0f; t1.z = pz;
    t1.x1 = px + 0.35f; t1.y1 = 1.4f; t1.z1 = pz + 0.2f;
    t1.r0 = 0.16f; t1.r1 = 0.12f;
    t1.r = 0.48f; t1.g = 0.36f; t1.b = 0.22f; t1.gloss = 0.08f;
    out.push_back(t1);
    Solid t2; t2.kind = SolidKind::Taper;
    t2.x = px + 0.35f; t2.y = 1.4f; t2.z = pz + 0.2f;
    t2.x1 = px + 0.65f; t2.y1 = 2.7f; t2.z1 = pz + 0.35f;
    t2.r0 = 0.12f; t2.r1 = 0.09f;
    t2.r = 0.48f; t2.g = 0.36f; t2.b = 0.22f; t2.gloss = 0.08f;
    out.push_back(t2);
    // 5 Palm Fronds (sails)
    const float top_x = px + 0.65f, top_y = 2.7f, top_z = pz + 0.35f;
    for (int f = 0; f < 5; ++f) {
      const float fa = (float)f * 6.2831853f / 5.0f;
      Solid frond; frond.kind = SolidKind::Sail;
      frond.x = top_x; frond.y = top_y; frond.z = top_z;
      frond.x1 = top_x + std::cos(fa) * 1.4f; frond.y1 = top_y - 0.35f; frond.z1 = top_z + std::sin(fa) * 1.4f;
      frond.x2 = top_x + std::cos(fa + 0.35f) * 1.2f; frond.y2 = top_y - 0.55f; frond.z2 = top_z + std::sin(fa + 0.35f) * 1.2f;
      frond.r0 = 0.025f;
      frond.r = 0.18f; frond.g = 0.52f; frond.b = 0.16f; frond.gloss = 0.25f;
      out.push_back(frond);
    }
  }
}

// Giant Mushrooms
void compose_giant_mushrooms(std::vector<Solid>& out, int n) {
  for (int i = 0; i < n; ++i) {
    const float a = (float)i * 6.2831853f / (float)std::max(1, n);
    const float dist = n > 1 ? 2.2f : 0.0f;
    const float mx = std::cos(a) * dist, mz = std::sin(a) * dist;
    const float h = 1.4f + 0.4f * (float)(i % 3);
    // Stem (taper)
    Solid stem; stem.kind = SolidKind::Taper;
    stem.x = mx; stem.y = 0.0f; stem.z = mz;
    stem.x1 = mx + 0.15f; stem.y1 = h; stem.z1 = mz + 0.1f;
    stem.r0 = 0.22f; stem.r1 = 0.16f;
    stem.r = 0.88f; stem.g = 0.85f; stem.b = 0.78f; stem.gloss = 0.15f;
    out.push_back(stem);
    // Cap (ellipsoid)
    Solid cap; cap.kind = SolidKind::Ellipsoid;
    cap.x = mx + 0.15f; cap.y = h + 0.2f; cap.z = mz + 0.1f;
    cap.sx = 0.85f; cap.sy = 0.42f; cap.sz = 0.85f;
    cap.r = (i % 2 == 0) ? 0.85f : 0.32f;
    cap.g = (i % 2 == 0) ? 0.18f : 0.55f;
    cap.b = (i % 2 == 0) ? 0.14f : 0.82f;
    cap.gloss = 0.45f;
    out.push_back(cap);
  }
}

}  // namespace

// Composition on demand. This is called from ONE place — the `display` command
// handling a request somebody typed or asked for — and must stay that way: the
// composer answers a request, it never decides that a new scene is wanted. Who
// owns the choice of what is on the screen is the person making the request.
SceneDescription compose_scene(const std::string& request) {
  const std::string q = lower(request);
  Rng rng(seed_of(q));
  SceneDescription d;
  d.solids.clear();

  std::string sky_words;
  std::string ground_words;
  // Whether the request actually NAMED these, as against getting the default.
  // It decides what to do when no object was asked for: "the ocean at dawn" is
  // a complete scene with nothing standing in it, and inventing a solid there
  // is the machine adding something nobody asked for.
  bool sky_named = false;
  bool ground_named = false;

  // ---- the sky, and with it the light -------------------------------------
  if (mentions(q, "night") || mentions(q, "midnight") || mentions(q, "starlit") ||
      mentions(q, "stars") || mentions(q, "moonlit")) {
    set3(d.sky_low, 0.035f, 0.055f, 0.115f);
    set3(d.sky_high, 0.008f, 0.014f, 0.038f);
    set3(d.sun_dir, -0.45f, 0.38f, -0.62f);
    set3(d.sun_colour, 0.72f, 0.78f, 0.95f);
    d.sun_intensity = 0.42f; d.sun_sharpness = 130.0f;
    set3(d.fog_colour, 0.03f, 0.05f, 0.10f);
    d.fog_density = 0.020f;
    d.ambient = 0.34f;
    sky_words = "at night";
    sky_named = true;
  } else if (mentions(q, "sunset") || mentions(q, "dusk") || mentions(q, "evening") ||
             mentions(q, "golden hour")) {
    set3(d.sky_low, 0.86f, 0.44f, 0.20f);
    set3(d.sky_high, 0.14f, 0.16f, 0.42f);
    set3(d.sun_dir, 0.92f, 0.12f, -0.20f);
    set3(d.sun_colour, 1.0f, 0.66f, 0.34f);
    d.sun_intensity = 1.15f; d.sun_sharpness = 26.0f;
    set3(d.fog_colour, 0.62f, 0.38f, 0.26f);
    d.fog_density = 0.042f;
    d.ambient = 0.16f;
    sky_words = "at sunset";
    sky_named = true;
  } else if (mentions(q, "dawn") || mentions(q, "sunrise") || mentions(q, "morning")) {
    set3(d.sky_low, 0.78f, 0.56f, 0.52f);
    set3(d.sky_high, 0.16f, 0.32f, 0.56f);
    set3(d.sun_dir, -0.85f, 0.18f, 0.30f);
    set3(d.sun_colour, 1.0f, 0.80f, 0.62f);
    d.sun_intensity = 1.0f; d.sun_sharpness = 34.0f;
    set3(d.fog_colour, 0.60f, 0.54f, 0.56f);
    d.fog_density = 0.050f;
    d.ambient = 0.20f;
    sky_words = "at dawn";
    sky_named = true;
  } else if (mentions(q, "storm") || mentions(q, "thunder") || mentions(q, "overcast") ||
             mentions(q, "rain") || mentions(q, "fog") || mentions(q, "mist")) {
    set3(d.sky_low, 0.30f, 0.32f, 0.35f);
    set3(d.sky_high, 0.14f, 0.16f, 0.19f);
    set3(d.sun_dir, 0.30f, 0.72f, -0.30f);
    set3(d.sun_colour, 0.72f, 0.74f, 0.78f);
    d.sun_intensity = 0.55f; d.sun_sharpness = 8.0f;
    set3(d.fog_colour, 0.26f, 0.28f, 0.31f);
    d.fog_density = 0.085f;
    d.ambient = 0.42f;
    sky_words = "under an overcast sky";
    sky_named = true;
  } else {
    set3(d.sky_low, 0.58f, 0.70f, 0.86f);
    set3(d.sky_high, 0.16f, 0.34f, 0.68f);
    set3(d.sun_dir, 0.72f, 0.52f, -0.24f);
    set3(d.sun_colour, 1.0f, 0.94f, 0.82f);
    d.sun_intensity = 1.10f; d.sun_sharpness = 52.0f;
    set3(d.fog_colour, 0.60f, 0.70f, 0.84f);
    d.fog_density = 0.028f;
    d.ambient = 0.18f;
    sky_words = "in daylight";
  }

  // ---- the ground ---------------------------------------------------------
  // The grounds WITH relief come first: "a river through the mountains" is a
  // river, and "mountains" is terrain, and either answer is better than the
  // flat plane both used to fall through to.
  if (mentions(q, "canyon") || mentions(q, "gorge") || mentions(q, "mesa") ||
      mentions(q, "ravine") || mentions(q, "badlands") || mentions(q, "plateau")) {
    d.ground = GroundKind::Canyon;
    set3(d.ground_colour, 0.65f, 0.32f, 0.18f);
    ground_words = "in a sandstone canyon";
    ground_named = true;
  } else if (mentions(q, "volcano") || mentions(q, "caldera") || mentions(q, "lava") ||
             mentions(q, "magma") || mentions(q, "crater") || mentions(q, "eruption")) {
    d.ground = GroundKind::Volcano;
    set3(d.ground_colour, 0.14f, 0.14f, 0.16f);
    ground_words = "at a volcanic caldera";
    ground_named = true;
  } else if (mentions(q, "glacier") || mentions(q, "icefield") || mentions(q, "crevasse") ||
             mentions(q, "ice sheet") || mentions(q, "antarctic") || mentions(q, "arctic ice")) {
    d.ground = GroundKind::Glacier;
    set3(d.ground_colour, 0.65f, 0.82f, 0.94f);
    ground_words = "across a glacier";
    ground_named = true;
  } else if (mentions(q, "ocean") || mentions(q, "deep sea") || mentions(q, "open water")) {
    d.ground = GroundKind::Ocean;
    set3(d.ground_colour, 0.02f, 0.08f, 0.18f);
    ground_words = "over the open ocean";
    ground_named = true;
  } else if (mentions(q, "river") || mentions(q, "stream") || mentions(q, "creek") ||
      mentions(q, "brook") || mentions(q, "estuary")) {
    d.ground = GroundKind::River;
    set3(d.ground_colour, 0.26f, 0.32f, 0.18f);
    ground_words = "along a river";
    ground_named = true;
  } else if (mentions(q, "mountain") || mentions(q, "hill") || mentions(q, "valley") ||
             mentions(q, "highland") || mentions(q, "ridge") || mentions(q, "alps") ||
             mentions(q, "peak")) {
    d.ground = GroundKind::Hills;
    set3(d.ground_colour, 0.30f, 0.30f, 0.27f);
    ground_words = "in the hills";
    ground_named = true;
  } else if (mentions(q, "dune") || mentions(q, "desert") || mentions(q, "sahara") ||
             mentions(q, "sands")) {
    d.ground = GroundKind::Dunes;
    set3(d.ground_colour, 0.76f, 0.64f, 0.42f);
    ground_words = "over dunes";
    ground_named = true;
  } else if (mentions(q, "beach") || mentions(q, "shore") || mentions(q, "coast") ||
             mentions(q, "seaside") || mentions(q, "surf")) {
    // A beach was not in this vocabulary at all, so "a beach at dawn" fell all
    // the way through to a bare plain — the machine answering a shoreline with
    // an empty field. It is a water plane seen from the land's edge.
    d.ground = GroundKind::Water;
    set3(d.ground_colour, 0.045f, 0.115f, 0.175f);
    ground_words = "at the shoreline";
    ground_named = true;
  } else if (mentions(q, "sea") || mentions(q, "water") || mentions(q, "lake") ||
             mentions(q, "tide") || mentions(q, "wave")) {
    d.ground = GroundKind::Water;
    set3(d.ground_colour, 0.045f, 0.115f, 0.175f);
    ground_words = "over water";
    ground_named = true;
  } else if (mentions(q, "space") || mentions(q, "orbit") || mentions(q, "void") ||
             mentions(q, "nebula") || mentions(q, "floating")) {
    d.ground = GroundKind::None;
    ground_words = "with no ground at all";
    ground_named = true;
  } else if (mentions(q, "snow") || mentions(q, "ice") || mentions(q, "arctic") ||
             mentions(q, "tundra")) {
    d.ground = GroundKind::Plain;
    set3(d.ground_colour, 0.82f, 0.85f, 0.90f);
    ground_words = "on snow";
    ground_named = true;
  } else if (mentions(q, "grass") || mentions(q, "field") || mentions(q, "meadow") ||
             mentions(q, "forest") || mentions(q, "jungle") || mentions(q, "moss")) {
    d.ground = GroundKind::Plain;
    set3(d.ground_colour, 0.20f, 0.34f, 0.16f);
    ground_words = "on grass";
    ground_named = true;
  } else if (mentions(q, "mars") || mentions(q, "rust")) {
    d.ground = GroundKind::Plain;
    set3(d.ground_colour, 0.44f, 0.22f, 0.14f);
    ground_words = "on red rock";
    ground_named = true;
  } else if (mentions(q, "city") || mentions(q, "street") || mentions(q, "asphalt")) {
    d.ground = GroundKind::Plain;
    set3(d.ground_colour, 0.16f, 0.16f, 0.18f);
    ground_words = "on dark ground";
    ground_named = true;
  } else if (mentions(q, "stone") || mentions(q, "rock") || mentions(q, "slate")) {
    d.ground = GroundKind::Plain;
    set3(d.ground_colour, 0.34f, 0.34f, 0.36f);
    ground_words = "on stone";
    ground_named = true;
  } else {
    d.ground = GroundKind::Plain;
    set3(d.ground_colour, 0.46f, 0.38f, 0.28f);
    ground_words = "on a plain";
  }

  // ---- what is standing on it --------------------------------------------
  float tint[3] = {0.0f, 0.0f, 0.0f};
  const bool tinted = colour_in(q, tint);
  std::string subject;

  auto proto_of = [&](SolidKind k, float sx, float sy, float sz, float r, float g, float b, float gloss) {
    Solid s; s.kind = k; s.sx = sx; s.sy = sy; s.sz = sz;
    if (tinted) { s.r = tint[0]; s.g = tint[1]; s.b = tint[2]; }
    else        { s.r = r; s.g = g; s.b = b; }
    s.gloss = gloss;
    return s;
  };

  if (mentions(q, "dragon") || mentions(q, "wyvern") || mentions(q, "drake")) {
    // A creature, not an arrangement: laid out as a skeleton with flesh hung
    // between its joints. See compose_dragon.
    Hide hide{0.24f, 0.30f, 0.20f, 0.22f};
    if (colour_in(q, &hide.r)) { /* the request named a colour; it is the hide */ }
    // The web is the hide lit through: thinner, warmer, and glossier, which is
    // what a membrane with light behind it looks like.
    Hide web{std::min(1.0f, hide.r * 1.25f + 0.10f),
             std::min(1.0f, hide.g * 1.05f + 0.06f),
             std::min(1.0f, hide.b * 1.05f + 0.06f), 0.30f};
    compose_dragon(d.solids, hide, web);
    d.camera_radius = 10.0f; d.camera_height = 3.0f; d.camera_fov = 0.66f;
    subject = "a dragon";
  } else if (mentions(q, "castle") || mentions(q, "fortress") || mentions(q, "citadel") ||
             mentions(q, "keep") || mentions(q, "stronghold")) {
    compose_castle(d.solids, tinted ? tint[0] : 0.62f, tinted ? tint[1] : 0.60f, tinted ? tint[2] : 0.58f);
    d.camera_radius = 9.5f; d.camera_height = 3.2f;
    subject = "a fortified castle";
  } else if (mentions(q, "spaceship") || mentions(q, "starfighter") || mentions(q, "shuttle") ||
             mentions(q, "spacecraft") || mentions(q, "vessel") || mentions(q, "cruiser")) {
    compose_spaceship(d.solids, tinted ? tint[0] : 0.82f, tinted ? tint[1] : 0.84f, tinted ? tint[2] : 0.90f);
    d.camera_radius = 6.8f; d.camera_height = 2.4f;
    subject = "a starfighter";
  } else if (mentions(q, "robot") || mentions(q, "mech") || mentions(q, "golem") ||
             mentions(q, "android") || mentions(q, "automaton")) {
    compose_mech(d.solids, tinted ? tint[0] : 0.35f, tinted ? tint[1] : 0.38f, tinted ? tint[2] : 0.44f);
    d.camera_radius = 7.5f; d.camera_height = 2.6f;
    subject = "a mech";
  } else if (mentions(q, "crystal") || mentions(q, "crystals") || mentions(q, "gem") ||
             mentions(q, "geode") || mentions(q, "quartz") || mentions(q, "amethyst")) {
    compose_crystals(d.solids, tinted ? tint[0] : 0.65f, tinted ? tint[1] : 0.25f, tinted ? tint[2] : 0.85f);
    d.camera_radius = 6.2f; d.camera_height = 2.2f;
    subject = "a crystal cluster";
  } else if (mentions(q, "pagoda") || mentions(q, "shrine") || mentions(q, "temple")) {
    compose_pagoda(d.solids, tinted ? tint[0] : 0.72f, tinted ? tint[1] : 0.22f, tinted ? tint[2] : 0.16f);
    d.camera_radius = 8.5f; d.camera_height = 2.8f;
    subject = "a pagoda";
  } else if (mentions(q, "palm") || mentions(q, "oasis")) {
    int n = count_in(q, true);
    compose_palm_trees(d.solids, n);
    d.camera_radius = 7.8f; d.camera_height = 2.5f;
    subject = std::to_string(n) + (n == 1 ? " palm tree" : " palm trees");
  } else if (mentions(q, "mushroom") || mentions(q, "mushrooms") || mentions(q, "toadstool") || mentions(q, "fungus")) {
    int n = count_in(q, true);
    compose_giant_mushrooms(d.solids, n);
    d.camera_radius = 6.5f; d.camera_height = 2.2f;
    subject = std::to_string(n) + (n == 1 ? " giant mushroom" : " giant mushrooms");
  } else if (mentions(q, "pyramid")) {
    int n = count_in(q, mentions(q, "pyramids"));
    place_ring(rng, d.solids, proto_of(SolidKind::Pyramid, 1.25f, 1.25f, 1.25f,
                                       0.74f, 0.64f, 0.46f, 0.05f),
               n, n > 1 ? 2.6f : 0.0f);
    subject = n > 1 ? std::to_string(n) + " pyramids" : "a pyramid";
  } else if (mentions(q, "mountain")) {
    // The GROUND is the mountains now — ridged terrain with its own skyline,
    // snow and shadows. Standing pyramids on it as well, which is what this
    // did when a mountain was a solid or nothing, puts two grey tetrahedra in
    // the middle of a mountain range.
    subject = mentions(q, "mountains") ? "mountains" : "a mountain";
    ground_named = true;
  } else if (mentions(q, "city") || mentions(q, "skyline") || mentions(q, "building")) {
    const int n = 11;
    for (int i = 0; i < n; ++i) {
      Solid s = proto_of(SolidKind::Box, rng.range(0.28f, 0.52f), rng.range(0.7f, 2.6f),
                         rng.range(0.28f, 0.52f), 0.22f, 0.23f, 0.27f, 0.35f);
      s.x = rng.range(-3.4f, 3.4f);
      s.z = rng.range(-3.4f, 3.4f);
      s.y = s.sy;
      d.solids.push_back(s);
    }
    d.camera_radius = 7.6f; d.camera_height = 2.2f;
    subject = "a skyline of " + std::to_string(n) + " towers";
  } else if (mentions(q, "tree") || mentions(q, "forest") || mentions(q, "wood")) {
    // count_in's plural default is 3, which is a copse and not a forest. Only
    // an explicit number should give a small one.
    bool counted = false;
    for (const auto& c : kCounts) if (mentions(q, c.word)) { counted = true; break; }
    int n = counted ? count_in(q, true) : 9;
    if (n < 1) n = 9;
    const float turn = 6.2831853f / (float)n;
    const float phase = rng.unit() * 6.2831853f;
    for (int i = 0; i < n; ++i) {
      const float a = phase + turn * (float)i;
      const float rr = 3.0f * rng.range(0.55f, 1.25f);
      const float h = rng.range(0.55f, 0.95f);
      Solid trunk = proto_of(SolidKind::Cylinder, 0.075f, h, 0.075f, 0.26f, 0.18f, 0.11f, 0.04f);
      trunk.x = std::cos(a) * rr; trunk.z = std::sin(a) * rr; trunk.y = h;
      // A trunk is never the colour you asked for; the canopy is.
      trunk.r = 0.26f; trunk.g = 0.18f; trunk.b = 0.11f;
      d.solids.push_back(trunk);
      Solid crown = proto_of(SolidKind::Sphere, rng.range(0.34f, 0.52f), 1.0f, 1.0f,
                             0.16f, 0.36f, 0.16f, 0.06f);
      crown.x = trunk.x; crown.z = trunk.z; crown.y = h * 2.0f + crown.sx * 0.55f;
      d.solids.push_back(crown);
    }
    subject = std::to_string(n) + " trees";
  } else if (mentions(q, "pine") || mentions(q, "fir") || mentions(q, "spruce") ||
             mentions(q, "conifer")) {
    // A conifer is a cone on a trunk, which is what the cone was added for:
    // the round-crowned tree above was the only tree this machine could grow.
    int n = count_in(q, true);
    const float turn = 6.2831853f / (float)std::max(1, n);
    const float phase = rng.unit() * 6.2831853f;
    for (int i = 0; i < n; ++i) {
      const float a = phase + turn * (float)i;
      const float rr = 3.2f * rng.range(0.55f, 1.25f);
      const float trunk_h = rng.range(0.22f, 0.38f);
      Solid trunk = proto_of(SolidKind::Cylinder, 0.07f, trunk_h, 0.07f, 0.24f, 0.17f, 0.10f, 0.04f);
      trunk.x = std::cos(a) * rr; trunk.z = std::sin(a) * rr; trunk.y = trunk_h;
      d.solids.push_back(trunk);
      Solid crown = proto_of(SolidKind::Cone, rng.range(0.42f, 0.62f), rng.range(1.5f, 2.4f), 1.0f,
                             0.13f, 0.30f, 0.17f, 0.05f);
      crown.x = trunk.x; crown.z = trunk.z; crown.y = trunk_h * 2.0f;
      d.solids.push_back(crown);
    }
    subject = std::to_string(n) + (n == 1 ? " pine" : " pines");
  } else if (mentions(q, "cone") || mentions(q, "spire") || mentions(q, "tent")) {
    int n = count_in(q, mentions(q, "cones") || mentions(q, "spires") || mentions(q, "tents"));
    place_ring(rng, d.solids, proto_of(SolidKind::Cone, 0.8f, 1.9f, 0.8f,
                                       0.70f, 0.66f, 0.58f, 0.10f),
               n, 3.0f);
    subject = std::to_string(n) + (n == 1 ? " cone" : " cones");
  } else if (mentions(q, "sphere") || mentions(q, "orb") || mentions(q, "ball") ||
             mentions(q, "planet") || mentions(q, "bubble") || mentions(q, "moon")) {
    int n = count_in(q, mentions(q, "spheres") || mentions(q, "orbs") || mentions(q, "balls"));
    place_ring(rng, d.solids, proto_of(SolidKind::Sphere, 0.85f, 0.85f, 0.85f,
                                       0.66f, 0.68f, 0.74f, 0.55f),
               n, n > 1 ? 2.2f : 0.0f);
    if (n == 1) d.solids[0].y = 0.85f;
    subject = n > 1 ? std::to_string(n) + " spheres" : "a sphere";
  } else if (mentions(q, "monolith") || mentions(q, "cube") || mentions(q, "box") ||
             mentions(q, "block") || mentions(q, "slab")) {
    int n = count_in(q, mentions(q, "cubes") || mentions(q, "blocks"));
    const bool tall = mentions(q, "monolith");
    place_ring(rng, d.solids,
               proto_of(SolidKind::Box, tall ? 0.34f : 0.75f, tall ? 1.9f : 0.75f,
                        tall ? 0.12f : 0.75f, 0.18f, 0.18f, 0.20f, 0.28f),
               n, n > 1 ? 2.3f : 0.0f);
    // The count was dropped for monoliths, so seven of them described
    // themselves as "a monolith" while seven were standing there.
    if (tall) subject = (n > 1) ? (std::to_string(n) + " monoliths") : std::string("a monolith");
    else      subject = (n > 1) ? (std::to_string(n) + " cubes") : std::string("a cube");
  } else if (mentions(q, "torus") || mentions(q, "ring") || mentions(q, "halo") ||
             mentions(q, "arch") || mentions(q, "donut")) {
    int n = count_in(q, mentions(q, "rings"));
    Solid p2 = proto_of(SolidKind::Torus, 1.0f, 0.26f, 1.0f, 0.72f, 0.60f, 0.26f, 0.62f);
    p2.y = 1.25f;
    place_ring(rng, d.solids, p2, n, n > 1 ? 2.4f : 0.0f);
    subject = n > 1 ? std::to_string(n) + " rings" : "a ring";
  } else if (mentions(q, "tower") || mentions(q, "column") || mentions(q, "pillar") ||
             mentions(q, "obelisk") || mentions(q, "spire")) {
    int n = count_in(q, mentions(q, "towers") || mentions(q, "columns") || mentions(q, "pillars"));
    place_ring(rng, d.solids, proto_of(SolidKind::Cylinder, 0.30f, 1.7f, 0.30f,
                                       0.68f, 0.64f, 0.56f, 0.12f),
               n, n > 1 ? 2.7f : 0.0f);
    subject = n > 1 ? std::to_string(n) + " columns" : "a column";
  } else if (mentions(q, "boulder") || mentions(q, "rocks") || mentions(q, "stones")) {
    place_ring(rng, d.solids, proto_of(SolidKind::Sphere, 0.55f, 0.55f, 0.55f,
                                       0.38f, 0.36f, 0.34f, 0.05f),
               6, 2.8f);
    subject = "6 boulders";
  } else if (ground_named || sky_named) {
    // A place and a light, and nothing standing in it. Asked for "the ocean at
    // dawn" this machine used to add a solid it was never asked for, because
    // the fallback ran whenever no OBJECT was named — but the request had been
    // understood, and the answer is the empty horizon.
    subject = "an open horizon";
  } else {
    // Nothing this machine has a word for. It still composes: a form seeded by
    // the request, so the picture is specific to what was asked even when the
    // words were not understood. Refusing was the behaviour being fixed.
    static const SolidKind kAny[] = {SolidKind::Sphere, SolidKind::Box, SolidKind::Pyramid,
                                     SolidKind::Torus, SolidKind::Cylinder, SolidKind::Cone};
    const SolidKind k = kAny[rng.next() % 5];
    const int n = 1 + (int)(rng.next() % 4);
    Solid p2 = proto_of(k, 0.80f, k == SolidKind::Cylinder ? 1.4f : 0.80f, 0.80f,
                        rng.range(0.25f, 0.80f), rng.range(0.25f, 0.80f), rng.range(0.25f, 0.80f),
                        rng.range(0.05f, 0.60f));
    if (k == SolidKind::Torus) { p2.sy = 0.24f; p2.y = 1.1f; }
    place_ring(rng, d.solids, p2, n, n > 1 ? 2.4f : 0.0f);
    subject = std::string(n > 1 ? std::to_string(n) + " " : "a ") + solid_kind_name(k) +
              (n > 1 ? "s" : "") + " composed from the words given";
  }

  // Frame it from what was actually placed, rather than from a guess made
  // before placing anything. A fixed radius cropped the tall things and left
  // the small ones as specks: the composer does not know in advance how big
  // the scene it is about to build will be, so it measures it afterwards.
  float extent = 0.0f;
  for (const auto& sol : d.solids) {
    const float reach = std::sqrt(sol.x * sol.x + sol.z * sol.z);
    const float bulk = std::max(sol.sx, std::max(sol.sy, sol.sz));
    const float top = sol.y + bulk;
    extent = std::max(extent, std::max(reach + bulk, top));
  }
  if (extent > 0.0f) {
    d.camera_radius = std::max(2.2f, extent * 1.95f);
    d.camera_height = 0.50f + extent * 0.42f;
  } else {
    // Nothing standing in it: the subject is the horizon, so sit low and look
    // out rather than down at an empty plane from height.
    d.camera_radius = 4.0f;
    d.camera_height = 1.1f;
  }
  // Ground with relief is the subject in its own right and it is BIG — hills
  // stand several units tall, which a camera framed for a sphere on a plane is
  // standing inside rather than looking at. Pull back and climb.
  if (d.ground == GroundKind::Hills || d.ground == GroundKind::Dunes ||
      d.ground == GroundKind::River || d.ground == GroundKind::Canyon ||
      d.ground == GroundKind::Volcano || d.ground == GroundKind::Glacier) {
    d.camera_radius = std::max(d.camera_radius, 11.0f);
    d.camera_height = std::max(d.camera_height, 4.2f);
  }
  // A little variation in where it is watched from, still seeded by the words.
  d.camera_radius *= rng.range(0.94f, 1.14f);
  d.camera_spin = rng.range(0.05f, 0.14f);

  d.summary = subject + " " + ground_words + " " + sky_words;
  return d;
}

float time_of_day_of(const std::string& request, bool* named) {
  const std::string q = lower(request);
  if (named) *named = true;
  // Ordered so the SPECIFIC hour wins over the half of the day containing it:
  // "sunset" and "dusk" both live inside "evening", and "midnight" inside
  // "night". The words are the composer's own, so the two agree about what a
  // request means even though only one of them ends up drawing it.
  if (mentions(q, "midnight")) return 0.0f;
  if (mentions(q, "sunrise") || mentions(q, "dawn") || mentions(q, "daybreak") ||
      mentions(q, "first light")) return 0.25f;
  if (mentions(q, "morning")) return 0.33f;
  if (mentions(q, "midday") || mentions(q, "noon") || mentions(q, "daylight") ||
      mentions(q, "daytime") || mentions(q, "afternoon")) return 0.5f;
  if (mentions(q, "golden hour")) return 0.70f;
  if (mentions(q, "sunset") || mentions(q, "dusk") || mentions(q, "twilight") ||
      mentions(q, "evening")) return 0.78f;
  if (mentions(q, "night") || mentions(q, "starlit") || mentions(q, "moonlit") ||
      mentions(q, "moonlight") || mentions(q, "stars") || mentions(q, "moon"))
    return 0.0f;
  if (named) *named = false;
  return 0.0f;    // the hour this scene has always been drawn at
}

}  // namespace aios::userland
