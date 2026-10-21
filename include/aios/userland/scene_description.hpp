#pragma once
// A scene this machine composed, rather than one it was compiled with.
//
// The display could only ever draw the four scenes somebody wrote a function
// for: asked for a forest it said it did not know how, which is the difference
// between a machine that renders and a machine that owns four pictures. A
// description is data — a sky, a ground, a sun and a list of solids — so the
// renderer is one raymarcher over whatever it is handed and the machine's job
// is to fill the description in.
//
// It crosses a PROCESS boundary. `display show` runs in the resident or a
// console kernel; the compositor that draws is a different process, and the
// two already speak through a file in the run directory. So this serialises to
// text: a scene you cannot read back in a terminal is a scene nobody can
// debug, and the existing scene file is one word of plain text for the same
// reason.
#include <cstdint>
#include <string>
#include <vector>
#include "aios/core/result.hpp"

namespace aios::userland {

// What the raymarcher knows how to evaluate a distance to. Adding one means
// adding a case in the shader; the vocabulary is deliberately small, because a
// composer that can place five solids well is worth more than one that can
// name twenty badly.
enum class SolidKind : uint32_t {
  Sphere     = 0,
  Box        = 1,
  Pyramid    = 2,
  Torus      = 3,
  Cylinder   = 4,
  Cone       = 5,
  Capsule    = 6,   // p0 -> p1, one radius
  Taper      = 7,   // p0 -> p1, radius at each end
  Sail       = 8,   // a triangle with thickness: a wing membrane, a fin, a leaf
  HexPrism   = 9,   // hexagonal prism (crystals, columns, pylons)
  Ellipsoid  = 10,  // 3-axis ellipsoid (smooth bodies, organic forms, stones)
  Ring       = 11,  // flat disc / planetary ring / halo
  Octahedron = 12,  // 8-sided gemstone / crystal / sci-fi node
  Arch       = 13,  // architectural arch / portal / bridge
  Star       = 14,  // 3D stellated star / crystal cluster
};

const char* solid_kind_name(SolidKind k);
bool parse_solid_kind(const std::string& s, SolidKind& out);

enum class GroundKind : uint32_t {
  None    = 0,   // sky all the way down; for a scene that floats
  Plain   = 1,   // a plane, textured by the noise field
  Water   = 2,   // a plane with a swell on it and a sun column
  Hills   = 3,   // rolling terrain
  Dunes   = 4,   // longer, smoother swells, wind-shaped
  River   = 5,   // Terrain with a channel cut through it, filled to a water line
  Canyon  = 6,   // Terraced sandstone canyon with strata bands & dry gorge
  Volcano = 7,   // Conical caldera with glowing molten lava bed
  Glacier = 8,   // Jagged crystalline glacial ice field with crevasses
  Ocean   = 9,   // Deep dynamic ocean with multi-octave waves and foam crests
};

const char* ground_kind_name(GroundKind k);
bool parse_ground_kind(const std::string& s, GroundKind& out);

struct Solid {
  SolidKind kind = SolidKind::Sphere;
  float x = 0.0f, y = 1.0f, z = 0.0f;
  // Read per kind: sphere uses sx as radius; box uses all three as half
  // extents; pyramid uses sy as height and sx as half-base; torus uses sx as
  // the major and sy as the minor radius; cylinder uses sx as radius and sy as
  // half height.
  float sx = 1.0f, sy = 1.0f, sz = 1.0f;
  float r = 0.8f, g = 0.8f, b = 0.8f;
  // 0 is chalk, 1 is polished. The shader spends it on a specular lobe and on
  // how much sky the surface mirrors.
  float gloss = 0.1f;

  // The second point, for the kinds that are defined by a segment or a
  // triangle: Capsule and Taper read it as the far end, Sail as its second
  // corner. x/y/z is the first point for those kinds rather than a centre.
  float x1 = 0.0f, y1 = 0.0f, z1 = 0.0f;
  // Sail's third corner.
  float x2 = 0.0f, y2 = 0.0f, z2 = 0.0f;
  // Radius at each end (Capsule uses r0 only), and the thickness of a Sail.
  float r0 = 0.2f, r1 = 0.1f;
  // How far this part FUSES into the ones around it. A body is not a pile of
  // separate solids: at 0 the part keeps a hard edge, and above that it melds
  // over that distance, which is what turns a chain of tapers into an animal.
  float blend = 0.0f;
};

struct SceneDescription {
  // What the machine would say it drew, in words, so the console can report
  // the scene rather than a number.
  std::string summary;

  float sky_low[3]  = {0.62f, 0.47f, 0.34f};   // at the horizon
  float sky_high[3] = {0.16f, 0.26f, 0.48f};   // overhead
  float sun_dir[3]  = {0.72f, 0.44f, -0.24f};  // normalised on load
  float sun_colour[3] = {1.0f, 0.82f, 0.55f};
  float sun_intensity = 1.05f;
  float sun_sharpness = 48.0f;                 // how tight the disc in the sky is

  GroundKind ground = GroundKind::Plain;
  float ground_colour[3] = {0.42f, 0.34f, 0.26f};

  // How much light a surface gets with none of the sun on it. It was a
  // constant in the shader, which made a night scene unreadably black: at
  // night almost nothing is lit directly and the ambient IS the picture.
  float ambient = 0.20f;

  float fog_colour[3] = {0.5f, 0.55f, 0.65f};
  float fog_density = 0.030f;

  float camera_radius = 3.6f;
  float camera_height = 1.75f;
  float camera_fov = 0.60f;
  float camera_spin = 0.09f;    // radians of drift; 0 holds still

  std::vector<Solid> solids;

  // The layout the shader reads: vec4-aligned throughout, because std430 with
  // mixed scalars is where a buffer silently disagrees with its shader.
  // Eight of header, then one more holding the bounding sphere around every
  // solid — the shader answers a point outside it with the distance to it
  // rather than testing each part, which is what makes a model of many parts
  // cost what a ray actually looks at.
  static constexpr size_t kHeaderVec4s = 9;
  // Five: kind and first point, size and blend, colour and gloss, then the
  // second and third points with a radius each — what a segment or a triangle
  // needs to carry its own direction.
  static constexpr size_t kSolidVec4s = 5;
  std::vector<float> to_floats() const;

  std::string to_text() const;
  static Result<SceneDescription> from_text(const std::string& text);
};

// The composer: what this machine makes of a request in words. Deterministic —
// the same words give the same scene, because a display that draws something
// different every time you ask for the same thing is not answering, it is
// guessing. Unknown words still produce a scene: the point is that there is no
// request it refuses.
SceneDescription compose_scene(const std::string& request);

// What hour of the day a request names, as a fraction: 0 midnight, 0.25 dawn,
// 0.5 midday, 0.75 dusk. `named` says whether any word in the request actually
// asked for an hour, so a caller can tell "at night" from a request that never
// mentioned the time.
//
// The compiled-in shoreline (scene 0) reads this. It used to be a night scene
// and nothing else — every colour in it a literal — so "the beach at dusk" had
// to be handed to the composed renderer, which draws a decent sea and sky but
// has no sand, no wash, no surf line and no palm. The scene the person asked
// for stays; only the light moves.
//
// One vocabulary, because `display show` writes the word and the compositor
// reads it back out of the run directory: two lists would drift, and the drift
// would show up as a machine agreeing to an hour and then drawing another.
float time_of_day_of(const std::string& request, bool* named = nullptr);

}  // namespace aios::userland
