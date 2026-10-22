#include "aios/userland/canvas.hpp"
#include "aios/userland/key_bindings.hpp"
#include "aios/userland/compositor.hpp"
#include "aios/userland/font_atlas.hpp"
#include "aios/userland/glyph_engine.hpp"
#include "aios/userland/directive.hpp"
#include "aios/userland/horizon.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/intelligence/intelligence.hpp"
#include "aios/intelligence/world_model.hpp"
#include "aios/compute/device.hpp"
#include "aios/userland/scene_description.hpp"
#include "aios/userland/button_suite.hpp"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#if !defined(AIOS_METAL)
#include <fstream>  // host-only: write_ppm below
#endif

namespace aios::userland {

// The run directory: AOS_RUN when a machine is told where it is, otherwise
// inside this machine's own root — which is the convention `face look` already
// writes its picture under. No host's absolute path appears here, because the
// literal that used to be here was one particular machine's and every other
// machine wrote its scenes somewhere nothing read.
std::string run_dir(Kernel& k) {
  const char* env = getenv("AOS_RUN");
  if (env && *env) return std::string(env);
  const std::string root = k.config().root;
  return root.empty() ? std::string("run") : root + "/run";
}

namespace {

// The console's own palette, so this machine looks like one machine.
constexpr uint32_t kBg = 0xFF0A0A0A, kPanel = 0xFF171717, kCard = 0xFF101010;
constexpr uint32_t kText = 0xFFF4F4F4, kDim = 0xFFB2B2B2, kFaint = 0xFF8E8E8E;
constexpr uint32_t kBorder = 0xFF262626, kStrong = 0xFF3A3A3A;
constexpr uint32_t kOk = 0xFF4ADE80, kWarn = 0xFFFACC15, kErr = 0xFFF87171, kMint = 0xFF7DD3FC, kViolet = 0xFFA855F7;

std::string dec(uint64_t v) {
  if (!v) return "0";
  std::string s;
  while (v) { s.insert(s.begin(), (char)('0' + v % 10)); v /= 10; }
  return s;
}
std::string ms_short(uint64_t ms) {
  if (ms < 1000) return dec(ms) + "ms";
  if (ms < 60000) return dec(ms / 1000) + "s";
  if (ms < 3600000) return dec(ms / 60000) + "m";
  if (ms < 86400000ull) return dec(ms / 3600000) + "h";
  return dec(ms / 86400000ull) + "d";
}
// How many bytes the UTF-8 sequence starting at `i` occupies, clamped to what
// the string actually has. One function, because four places used to work this
// out inline and one of them got it wrong.
size_t utf8_len(const std::string& s, size_t i) {
  const unsigned char c = (unsigned char)s[i];
  size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
  if (i + len > s.size()) len = 1;
  return len;
}
// The code point that sequence stands for, or 0 for a byte that is not a
// sequence head.
uint32_t utf8_cp(const std::string& s, size_t i, size_t len) {
  const unsigned char c = (unsigned char)s[i];
  if (len == 1) return c < 0x80 ? c : 0;
  uint32_t cp = c & (len == 2 ? 0x1Fu : len == 3 ? 0x0Fu : 0x07u);
  for (size_t e = 1; e < len; ++e) cp = (cp << 6) | ((unsigned char)s[i + e] & 0x3Fu);
  return cp;
}
// Can the faces draw this code point? Asked of the atlas rather than assumed,
// so widening the atlas widens what reaches the glass with no second list to
// keep in step. Every face is cut from the same coverage set; UI and MONO are
// the two typefaces behind it, so agreeing on both is agreeing on all twelve.
bool renderable(uint32_t cp) {
  if (cp == 0) return false;
  if (fonts::UI.find(cp) != nullptr && fonts::MONO.find(cp) != nullptr) return true;
  // Past the baked block the outline engine cuts the glyph on demand, so the
  // question is no longer "is it in the atlas" but "can this machine draw it
  // at all". Both faces are asked for the same reason as above: a character
  // the UI can draw and the mono cannot would come out of one pane and not
  // the other.
  return glyphs::rasterize(fonts::UI, cp) != nullptr &&
         glyphs::rasterize(fonts::MONO, cp) != nullptr;
}
// Truncating by BYTES would cut a multi-byte letter in half and leave a
// dangling continuation byte for the compositor to decode into nothing.
std::string clip(const std::string& s, size_t n) {
  if (s.size() <= n) return s;
  size_t cut = 0;
  while (cut < n - 1) {
    const size_t len = utf8_len(s, cut);
    if (cut + len > n - 1) break;
    cut += len;
  }
  return s.substr(0, cut) + "…";
}
// The faces carry ASCII glyphs only; typographic punctuation would reach the
// glass as garbage. Everything dynamic passes through here on its way to a pixel.
// The session log is a transcript, not a debug console. Two things were making
// it read as one: HTML entities coming back through the web engine as literal
// "&#x27;" in the middle of a sentence, and the raw object-store scan lines the
// horizon leaves behind ("? 670fa3ef3d8f 1788900108s 4e5f805beb01 0d ..."),
// which are an id, a timestamp, another id and a number — nothing a person
// reads. Returns false for a line that should not be shown at all.
bool readable_log_line(std::string& l) {
  // A store scan line: '?', a space, then a 12-hex-digit object id.
  if (l.size() > 14 && l[0] == '?' && l[1] == ' ') {
    bool hex = true;
    for (int i = 2; i < 14; ++i) {
      const char c = l[(size_t)i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { hex = false; break; }
    }
    if (hex) return false;
  }
  if (l.rfind("aios> __aos_think", 0) == 0) return false;
  static const struct { const char* from; const char* to; } kEntities[] = {
    {"&#x27;", "'"}, {"&#39;", "'"}, {"&quot;", "\""}, {"&#x2F;", "/"},
    {"&lt;", "<"}, {"&gt;", ">"}, {"&nbsp;", " "}, {"&amp;", "&"},
  };
  for (const auto& e : kEntities) {
    for (size_t at = l.find(e.from); at != std::string::npos; at = l.find(e.from, at)) {
      l.replace(at, std::strlen(e.from), e.to);
      at += std::strlen(e.to);
    }
  }
  return !l.empty();
}

std::string ascii(const std::string& in) {
  std::string o; o.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    unsigned char c = (unsigned char)in[i];
    if (c < 0x80) { o += (char)c; continue; }
    // UTF-8 sequences we deliberately SAY in ASCII, because a dash one pixel
    // different from a hyphen is noise on a status line. Anything else the
    // faces can actually draw goes through untouched — the atlas carries
    // Latin-1 and Latin Extended-A, so "Schrödinger" is a name, not "Schr?dinger".
    std::string seq = in.substr(i, utf8_len(in, i));
    i += seq.size() - 1;
    if (seq == "\xE2\x80\xA6") o += "...";          // …
    else if (seq == "\xC2\xB7") o += ".";             // ·
    else if (seq == "\xE2\x80\x94" || seq == "\xE2\x80\x93") o += "-";   // — –
    else if (seq == "\xE2\x80\x98" || seq == "\xE2\x80\x99") o += "'";   // ‘ ’
    else if (seq == "\xE2\x80\x9C" || seq == "\xE2\x80\x9D") o += "\""; // “ ”
    else if (seq == "\xE2\x86\x92") o += "->";       // →
    else if (seq == "\xE2\x80\xBA") o += ">";        // the single right-pointing quote the ask loop prints
    else if (seq == "\xE2\x80\xB9") o += "<";
    else if (renderable(utf8_cp(in, i - (seq.size() - 1), seq.size()))) o += seq;
    else o += "?";
  }
  return o;
}
// Is the pointer on this rect, in PAINT coordinates? The desk's own painter has
// a lambda for this; the tiles that live in their own functions need it too, and
// the scale is the ratio the painter itself picked — the screen over what it
// laid out for.
bool pointer_on(const HardwareCompositor& S, int iw, int x, int y, int w, int h) {
  const int scale = (iw > 0 && (int)S.width() / iw >= 2) ? 2 : 1;
  const int px = S.mouse_x() / scale, py = S.mouse_y() / scale;
  return px >= x && px < x + w && py >= y && py < y + h;
}

std::string upper(std::string s) { for (auto& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A'); return s; }

// The name of the intelligence THIS kernel actually has bound, asked of the
// kernel rather than of the environment. An unbound machine returns empty and
// every surface that reads this says so; the previous code read $AOS_MODEL and
// fell back to the literal string "FABLE", which is how a machine running
// nothing at all came to report a model by name on the glass.
}  // namespace

std::string bound_mind_name(Kernel& k) {
  for (const auto& e : k.entities().list()) {
    if (const MindBinding* b = k.mind(e.id)) {
      if (b->mind) return b->mind->name();
    }
  }
  return {};
}

std::string accelerator_brief(Kernel& k) {
  for (Device* d : k.devices().all()) {
    if (!d || d->type() != DeviceType::GPU) continue;
    std::string n = ascii(d->name());
    // "vulkan:NVIDIA GeForce GTX 1080 Ti api1.3" -> "GTX 1080 Ti". The backend
    // prefix is how it is reached, not what it is, and there is one bar.
    const size_t colon = n.find(':');
    if (colon != std::string::npos) n = n.substr(colon + 1);
    for (const char* drop : {"NVIDIA ", "GeForce ", "AMD ", "Radeon ", "Intel "}) {
      const size_t at = n.find(drop);
      if (at != std::string::npos) n.erase(at, std::string(drop).size());
    }
    // Trailing api/cc version the backend appends.
    const size_t sp = n.rfind(' ');
    if (sp != std::string::npos &&
        (n.compare(sp + 1, 3, "api") == 0 || n.compare(sp + 1, 2, "cc") == 0)) {
      n.erase(sp);
    }
    while (!n.empty() && n.back() == ' ') n.pop_back();
    return n;
  }
  return {};
}

namespace {

std::string fit(const HardwareCompositor& S, std::string t, int room, Face face) {
  if (room <= 0) return std::string();
  t = ascii(t);
  if (S.text_width(t, face) <= room) return t;
  // Forward, accumulating. Dropping the last character and re-measuring the
  // WHOLE string each time is quadratic in its length, and this is called on
  // every label of every row of every panel, every frame — with the brief's
  // own text among them, which is kilobytes long.
  const int ell = S.text_width("...", face);
  std::string kept;
  kept.reserve(t.size());
  int w = 0;
  for (size_t i = 0; i < t.size(); ) {
    // A letter at a time, not a byte at a time: measuring the lead byte of
    // "ö" alone is the wrong width, and stopping between its two bytes leaves
    // half a letter on the glass.
    const size_t len = utf8_len(t, i);
    const int cw = len == 1 ? S.glyph_advance(t[i], face)
                            : S.text_width(t.substr(i, len), face);
    if (w + cw + ell > room) break;
    kept.append(t, i, len);
    w += cw;
    i += len;
  }
  if (!kept.empty()) return kept + "...";
  if (S.text_width("..", face) <= room) return std::string("..");
  if (S.text_width(".", face) <= room) return std::string(".");
  return std::string();
}

}  // namespace

// What is new on this machine since the last look. Runs on the frame loop, so
// what it costs is what the face costs: it measured 133ms of a 150ms frame on a
// real machine -- the desktop sat at 7 FPS and none of it was painting.
//
// Two reasons, both fixed here:
//   - `already` walked a vector of every id ever seen, for every id, every
//     frame. Quadratic in the size of the store. It is a hash set now.
//   - the object loop did k.store().get(o) BEFORE asking whether o was already
//     known, so every frame read back every object in a 14MB store to build a
//     title that note() then threw away. Nothing is read now unless it is new.
void CanvasState::sweep(Kernel& k, uint64_t now_ms) {
  auto note = [&](const std::string& kind, const Id128& id, const std::string& what) {
    if (!known_ids.insert(id).second) return;   // already seen
    // What was already here when the canvas opened is history, not news.
    ledger.insert(ledger.begin(), Minted{kind, id.short_str(), what, id.str(), primed ? now_ms : 0});
    if (ledger.size() > 64) ledger.pop_back();
  };
  for (const auto& e : k.entities().list()) note("entity", e.id, e.label);
  for (const auto& s : k.states().list()) note("state", s.id, dec(s.payload.size()) + " bytes");
  for (const auto& c : k.channels().list()) note("channel", c.id, "");
  for (const auto& c : k.capabilities().list({})) note("capability", c.id, "");
  for (const auto& o : k.store().list()) {
    if (known_ids.count(o)) continue;           // ask before reading 14MB back
    auto got = k.store().get(o);
    note("knows", o, got.ok() ? clip(got->meta("title"), 34) : std::string());
  }
  primed = true;
}

CanvasFeed hosted_canvas_feed(Kernel& k) {
  CanvasFeed f;
  // count(), not list().size(): this label is built every frame, and listing
  // materialised (and sorted) every entity and every object id just to read
  // the size off it. Both registries carry the count already.
  f.right_label = upper(ms_short(k.uptime_ns() / 1000000ull)) + " UP   " + dec(k.entities().count()) + " ENTITIES   " +
                  dec(k.store().count()) + " OBJECTS";
  return f;
}

// A moonlit sea, painted the way a sea actually looks.
//
// What was here drew the water as one sine dashed across the screen, the
// moon's path as a single soft cone, the fronds as five straight wedges from
// an atan2, and the horizon as the line where one colour ramp stopped and the
// next began. Every one of those is a screen-space effect: nothing in the
// picture knew where the water WAS, so the waves were the same size at the
// horizon as at your feet and the moonlight sat on top of them rather than
// coming off them.
//
// This paints the sea in the world instead. A pixel below the horizon is a
// point on a plane, so its distance follows 1/(y - horizon) — that alone gives
// the wave texture its foreshortening for free, crowding to a fine shimmer at
// the horizon and opening out to metres across in the foreground. The waves
// are a sum of four travelling sines in world space, and the moon's path is
// the SPECULAR of that surface: a facet is lit when its slope happens to
// bisect the eye and the moon, which is why a real moon path is a broken
// column of separate glints that widens toward you, not a smooth cone.
//
// It is still a STILL — not one term reads the clock — so it is painted once
// per display size into the canvas' cache and copied from then on.
static inline double beach_hash01(uint32_t a, uint32_t b) {
  uint32_t h = a * 374761393u + b * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return (double)h / 4294967296.0;
}

// Value noise and an fbm over it, for the cloud deck. The shader draws its
// clouds from the same field turned between octaves — with a pure doubling
// every octave shares the same axes and the sum keeps the grid's own
// horizontals and verticals, which reads as tiling rather than as weather.
static inline double beach_noise(double x, double y) {
  const double fx = std::floor(x), fy = std::floor(y);
  const int ix = (int)fx, iy = (int)fy;
  double ux = x - fx, uy = y - fy;
  ux = ux * ux * (3.0 - 2.0 * ux);
  uy = uy * uy * (3.0 - 2.0 * uy);
  auto at = [](int a, int b) {
    return beach_hash01((uint32_t)(a + 65536), (uint32_t)(b + 65536));
  };
  const double a = at(ix, iy), b = at(ix + 1, iy);
  const double c = at(ix, iy + 1), d = at(ix + 1, iy + 1);
  return (a + (b - a) * ux) + ((c + (d - c) * ux) - (a + (b - a) * ux)) * uy;
}

static inline double beach_fbm(double x, double y) {
  double sum = 0.0, amp = 0.5;
  for (int i = 0; i < 5; ++i) {
    sum += amp * beach_noise(x, y);
    const double nx = 0.80 * x + 0.60 * y, ny = -0.60 * x + 0.80 * y;
    x = nx * 2.03; y = ny * 2.03; amp *= 0.5;
  }
  return sum;
}

static void render_beach_still(std::vector<uint32_t>& buf, int W, int H) {
  const double mcx = 0.72 * W;                 // the moon, and the light
  const double mcy = 0.17 * H;
  const double mr  = 0.062 * H;
  const double y_horizon = 0.46 * H;
  const double moon_far = mr * 7.0;            // past this the halo adds nothing
  const double moon_far2 = moon_far * moon_far;

  // The shore line varies with the column alone.
  std::vector<double> shore((size_t)W);
  for (int x = 0; x < W; ++x)
    shore[(size_t)x] = 0.74 * H + 0.028 * H * std::sin(x * 0.0055 + 0.5) +
                       0.012 * H * std::sin(x * 0.0145 + 2.1) +
                       0.005 * H * std::sin(x * 0.041 + 0.9);

  auto put = [&](uint32_t* row, int x, double r, double g, double b) {
    // One clamp, at the end, in floating point: the old painter added integers
    // channel by channel and every stage of it saturated separately, which is
    // what flattened the highlights into slabs of one colour.
    const int ri = (int)(std::clamp(r, 0.0, 1.0) * 255.0 + 0.5);
    const int gi = (int)(std::clamp(g, 0.0, 1.0) * 255.0 + 0.5);
    const int bi = (int)(std::clamp(b, 0.0, 1.0) * 255.0 + 0.5);
    row[x] = 0xFF000000u | ((uint32_t)ri << 16) | ((uint32_t)gi << 8) | (uint32_t)bi;
  };

  for (int y = 0; y < H; ++y) {
    uint32_t* row = &buf[(size_t)y * (size_t)W];
    const double dy = y - mcy;
    const double dy2 = dy * dy;
    const bool in_sky = y < y_horizon;

    // --- what varies with the row alone ---------------------------------
    // The night sky, warmed a little toward the horizon: air scatters the
    // moon's light along the longest path through it, which is the way you
    // are looking when you look out to sea.
    double sky_r = 0, sky_g = 0, sky_b = 0;
    if (in_sky) {
      const double t = (double)y / y_horizon;          // 0 zenith, 1 horizon
      const double lift = t * t * t;                   // stays dark until low
      sky_r = 0.012 + 0.085 * lift;
      sky_g = 0.028 + 0.115 * lift;
      sky_b = 0.070 + 0.165 * lift;
    }

    // Water rows: how far away this row is, and how big a wave looks there.
    const double yy = (double)y - y_horizon;
    const double depth = (yy > 0.0) ? (0.62 * H) / (yy + 0.9) : 0.0;

    for (int x = 0; x < W; ++x) {
      const double y_shore = shore[(size_t)x];
      double r, g, b;

      if (in_sky) {
        r = sky_r; g = sky_g; b = sky_b;

        // Stars: thinned out near the horizon haze and near the moon, where
        // the sky itself is bright enough to drown them.
        const double sky_t = (double)y / y_horizon;
        if (sky_t < 0.94) {
          const double u = beach_hash01((uint32_t)x, (uint32_t)y);
          if (u > 0.9988) {
            const double br = 0.35 + 0.65 * beach_hash01((uint32_t)y, (uint32_t)x);
            const double fade = (1.0 - sky_t * 0.75);
            r += br * fade * 0.85; g += br * fade * 0.9; b += br * fade;
          }
        }

        const double dx = x - mcx;
        const double d2 = dx * dx + dy2;
        if (d2 < moon_far2) {
          const double dist = std::sqrt(d2);
          if (dist <= mr) {
            // The disc: maria as broad soft patches, and a limb that darkens
            // the way a lit sphere does rather than a ring stuck on the edge.
            const double nx = dx / mr, ny = dy / mr;
            const double nz2 = 1.0 - nx * nx - ny * ny;
            const double nz = nz2 > 0.0 ? std::sqrt(nz2) : 0.0;
            double maria = 0.5 + 0.5 * std::sin(nx * 4.1 + 0.7) * std::cos(ny * 3.3 - 0.4) +
                           0.25 * std::sin((nx + ny) * 6.0);
            maria = std::clamp(maria, 0.0, 1.0);
            const double tone = 1.0 - 0.18 * maria;
            const double limb = 0.55 + 0.45 * std::pow(nz, 0.45);   // soft edge
            const double lit = tone * limb;
            r = 0.96 * lit; g = 0.965 * lit; b = 0.93 * lit;
            // one pixel of coverage at the rim so it is not a jagged circle
            const double cov = std::clamp(mr - dist, 0.0, 1.0);
            r = sky_r + (r - sky_r) * cov;
            g = sky_g + (g - sky_g) * cov;
            b = sky_b + (b - sky_b) * cov;
          } else {
            // Two-lobe halo: a tight corona and a wide bloom. One lobe reads
            // as a hard disc of glow with a visible edge.
            const double e = (dist - mr) / mr;
            const double tight = std::exp(-e * e * 1.6);
            const double wide  = std::exp(-e * 0.75);
            const double a = 0.55 * tight + 0.16 * wide;
            r += a * 0.62; g += a * 0.72; b += a * 0.88;
          }
        }

        // ---- the cloud deck ------------------------------------------
        // Cloud sits on a PLANE, so it crowds toward the horizon the same
        // way the sea does and for the same reason: a deck seen edge-on is
        // a hundred miles of it stacked into the last rows of pixels. A sky
        // that is a ramp with a dot in it is the one thing a night sky over
        // water never looks like.
        //
        // The device draws this deck MOVING, carried across that plane by a
        // wind. Here it is one instant of the same weather: this painter is
        // a still by construction — recomputing a 1920x1080 sea per frame
        // costs a third of a core — so a machine with no GPU gets the same
        // beach, held.
        {
          const double up = (y_horizon - y) / H;
          const double dist = 0.155 / (up + 0.013);
          const double cx = ((double)x / W - 0.5) * ((double)W / (double)H) * dist * 2.4;
          double f = beach_fbm(cx * 1.05, dist * 1.35 * 1.05);
          const double res = std::clamp(1.35 - dist * 0.055, 0.0, 1.0);
          f = 0.5 + (f - 0.5) * res;
          const double cover = std::clamp((f - 0.50) / 0.22, 0.0, 1.0) *
                               (0.55 + 0.45 * res);
          // The rim of a cloud is the bright part: that is where you are
          // looking through the least of it at the moon behind.
          const double rim = std::clamp(1.0 - std::fabs(f - 0.60) / 0.16, 0.0, 1.0);
          const double dist_moon = std::sqrt(dx * dx + dy2);
          const double moonlit = std::exp(-std::max(0.0, dist_moon - mr) / (0.38 * H));
          const double k = 0.16 + 0.84 * moonlit * (0.35 + 0.65 * rim);
          const double cr = 0.030 + (0.34 - 0.030) * k + 0.30 * moonlit * rim * 0.55;
          const double cg = 0.042 + (0.38 - 0.042) * k + 0.31 * moonlit * rim * 0.55;
          const double cb = 0.078 + (0.45 - 0.078) * k + 0.29 * moonlit * rim * 0.55;
          const double a = cover * 0.80;
          r += (cr - r) * a; g += (cg - g) * a; b += (cb - b) * a;
        }

      } else if (y < y_shore) {
        // ---- the sea ----------------------------------------------------
        // Where this pixel is on the water plane. wx is metres-ish across,
        // measured from the moon's azimuth so the glint maths is centred.
        const double wx = (x - mcx) * depth * (2.6 / H);

        // Four travelling swells. Every one of them runs mostly ALONG the
        // shore — the across-shore term is a tenth of the along-shore one —
        // because that is what a swell does: it arrives in lines parallel to
        // the beach and only leans a little. Weighting the two evenly instead
        // is what turned the sea into spokes radiating from a vanishing point.
        const double kz[6] = {0.115, 0.270, 0.640, 1.420, 3.10, 6.40};
        const double kx[6] = {0.055, -0.098, 0.165, -0.240, 0.420, -0.660};
        const double amp[6] = {0.50, 0.26, 0.14, 0.075, 0.048, 0.030};
        // How fast each octave's phase runs across a PIXEL at this distance.
        // An octave finer than the pixel it is being sampled at cannot be
        // drawn — sampling it anyway is what puts a fan of rays over the far
        // water, because the fine detail aliases along the lines of constant
        // world-x, and those lines all meet at the vanishing point. Fading an
        // octave out as it approaches a pixel wide is what a real horizon
        // does: you stop being able to resolve ripples miles away.
        const double dwx_dx = depth * (2.6 / H);
        const double ddepth_dy = -depth / (yy + 0.9);
        const double dwx_dy = -wx / (yy + 0.9);
        double h = 0.0, sx = 0.0, sz = 0.0;
        for (int n = 0; n < 6; ++n) {
          const double rate = std::abs(kx[n] * dwx_dx) +
                              std::abs(kz[n] * ddepth_dy + kx[n] * dwx_dy);
          const double fade = std::clamp(1.55 - rate * 2.6, 0.0, 1.0);
          if (fade <= 0.0) continue;
          const double a = amp[n] * fade;
          const double ph = depth * kz[n] + wx * kx[n];
          const double c = std::cos(ph);
          h  += a * std::sin(ph);
          sx += a * kx[n] * c;             // tilt across the line of sight
          sz += a * kz[n] * c;             // tilt toward the viewer
        }

        // Base water: dark, and picking up a little of the sky it faces.
        const double haze = std::exp(-yy / (0.055 * H));       // horizon mist
        double base_r = 0.020 + 0.030 * (0.5 + 0.5 * h);
        double base_g = 0.052 + 0.055 * (0.5 + 0.5 * h);
        double base_b = 0.098 + 0.080 * (0.5 + 0.5 * h);
        // Crests catch the sky; troughs are the sea's own colour.
        const double crest = std::clamp(h * 1.8, 0.0, 1.0);
        base_r += crest * 0.070; base_g += crest * 0.105; base_b += crest * 0.150;

        // ---- the moon's path, as specular off the actual facets ----------
        // A facet sends the moon to the eye when its slope matches the one
        // that bisects them. That required slope grows with how far off the
        // column this pixel is and shrinks with distance, which is exactly
        // why the path is a narrow shimmer at the horizon and a wide field of
        // separate glints at your feet.
        // The azimuth of a pixel relative to the moon depends on its COLUMN
        // and nothing else, which is why a moon path is a vertical band and
        // not a fan: making the required tilt grow with distance down the
        // screen drew the light as spokes out of a vanishing point.
        const double need_x = (x - mcx) / (0.30 * W);
        // The elevation half-way between the moon and the eye. Far off, the
        // eye looks level and a facet must tilt a long way to bridge them;
        // close in it is already looking down and barely any tilt will do. It
        // passes through zero at the point flat water would mirror the moon —
        // which is where a real path is at its brightest.
        const double need_z = 0.42 - 1.10 * (yy / H);
        const double miss_x = sx * 5.0 - need_x;
        const double miss_z = sz * 1.50 - need_z;
        const double glint = std::exp(-miss_x * miss_x * 9.0 - miss_z * miss_z * 13.0);
        const double column = std::exp(-need_x * need_x * 3.0);
        const double lit = glint * column;
        r = base_r + lit * 0.95;
        g = base_g + lit * 1.02;
        b = base_b + lit * 1.10;
        // A broad sheen under the column as well: the water there is simply
        // brighter, glints or no glints.
        r += column * 0.030; g += column * 0.038; b += column * 0.050;

        // The horizon is haze, not an edge.
        r = r * (1.0 - haze) + 0.075 * haze;
        g = g * (1.0 - haze) + 0.105 * haze;
        b = b * (1.0 - haze) + 0.160 * haze;

        // ---- the break and its foam --------------------------------------
        const double to_shore = y_shore - y;
        const double band = 0.085 * H;
        if (to_shore < band) {
          const double f = 1.0 - to_shore / band;             // 0 out, 1 at sand
          // The foam edge is ragged, and the ragged part is what sells it.
          const double n = 0.5 + 0.5 * std::sin(x * 0.055 + 1.3) *
                                 std::cos(x * 0.021 - 0.7) +
                           0.18 * std::sin(x * 0.19 + 2.2) +
                           0.10 * std::sin(x * 0.63 - 1.1) +
                           0.07 * std::sin(x * 1.37 + 0.3);
          const double edge = std::clamp((f - 0.72 + 0.20 * n) * 5.5, 0.0, 1.0);
          const double wash = std::clamp((f - 0.25) * 1.1, 0.0, 1.0);
          const double a = std::clamp(edge * 0.62 + wash * wash * 0.22, 0.0, 1.0);
          r += a * (0.62 - r) ; g += a * (0.72 - g); b += a * (0.80 - b);
        }

      } else {
        // ---- wet sand, then dry ------------------------------------------
        const double from_shore = y - y_shore;
        const double t = from_shore / std::max(1.0, (H - y_shore));
        const double wet = std::exp(-from_shore / (0.045 * H));   // still shining
        double sr = 0.085 + 0.215 * t;
        double sg = 0.072 + 0.170 * t;
        double sb = 0.058 + 0.118 * t;
        // Wet sand is darker, smoother, and it MIRRORS — the moon's column
        // carries on across it, dimmer and shorter.
        sr *= (1.0 - 0.45 * wet); sg *= (1.0 - 0.45 * wet); sb *= (1.0 - 0.40 * wet);
        const double need_x = (x - mcx) / (0.30 * W);
        const double column = std::exp(-need_x * need_x * 3.0);
        const double mirror = column * wet;
        sr += mirror * 0.30; sg += mirror * 0.34; sb += mirror * 0.40;
        // Grain, finer in the distance than at your feet.
        const double grain = (beach_hash01((uint32_t)x, (uint32_t)y) - 0.5) *
                             (0.030 + 0.035 * t);
        sr += grain; sg += grain; sb += grain;
        // The foam does not stop dead at the water's edge. Sea and sand are
        // different branches of an if, and without this the shore line is a
        // step in VALUE — which the device, marching the background at half
        // resolution, drew as a staircase the whole length of the beach.
        const double lip = std::clamp(1.0 - from_shore / (0.014 * H), 0.0, 1.0);
        const double la = lip * lip * (3.0 - 2.0 * lip) * 0.77;
        sr += (0.62 - sr) * la; sg += (0.72 - sg) * la; sb += (0.80 - sb) * la;
        r = sr; g = sg; b = sb;
      }

      put(row, x, r, g, b);
    }
  }

  // ---- the palm, drawn as curves rather than as a wedge of angles --------
  //
  // The fronds used to be five straight spokes from an atan2 test, which is
  // why they came out as triangles: a palm frond is a curve that falls away
  // under its own weight, and every leaflet on it hangs off that curve. This
  // walks each frond's spine and stamps the leaflets along it, so the shape
  // is the shape of the thing rather than the shape of the test.
  const double base_x = 0.085 * W, base_y = 0.80 * H;
  const double crown_x = 0.145 * W, crown_y = 0.205 * H;
  const uint32_t ink = 0xFF03060D;

  auto stamp = [&](double px, double py, double rad, double a) {
    if (a <= 0.01) return;
    const int x0 = std::max(0, (int)(px - rad - 1)), x1 = std::min(W - 1, (int)(px + rad + 1));
    const int y0 = std::max(0, (int)(py - rad - 1)), y1 = std::min(H - 1, (int)(py + rad + 1));
    for (int yy2 = y0; yy2 <= y1; ++yy2) {
      for (int xx = x0; xx <= x1; ++xx) {
        const double d = std::hypot(xx + 0.5 - px, yy2 + 0.5 - py);
        const double cov = std::clamp(rad + 0.5 - d, 0.0, 1.0) * a;
        if (cov <= 0.01) continue;
        uint32_t& p = buf[(size_t)yy2 * W + xx];
        const double ir = ((ink >> 16) & 0xFF) / 255.0, ig = ((ink >> 8) & 0xFF) / 255.0,
                     ib = (ink & 0xFF) / 255.0;
        const double br = ((p >> 16) & 0xFF) / 255.0, bg = ((p >> 8) & 0xFF) / 255.0,
                     bb = (p & 0xFF) / 255.0;
        const int nr = (int)((br + (ir - br) * cov) * 255.0 + 0.5);
        const int ng = (int)((bg + (ig - bg) * cov) * 255.0 + 0.5);
        const int nb = (int)((bb + (ib - bb) * cov) * 255.0 + 0.5);
        p = 0xFF000000u | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | (uint32_t)nb;
      }
    }
  };

  // Trunk: a quadratic that leans away from the wind, thick at the sand and
  // thin at the crown, with the rings of the bark showing as slight swells.
  {
    const double bend = 0.055 * W;
    const int steps = (int)(base_y - crown_y) * 2;
    for (int i = 0; i <= steps; ++i) {
      const double u = (double)i / steps;
      const double px = base_x + (crown_x - base_x) * u - bend * (u - u * u) * 4.0;
      const double py = base_y + (crown_y - base_y) * u;
      const double thick = (0.0055 * W) * (1.0 - 0.45 * u) *
                           (1.0 + 0.10 * std::sin(u * 46.0));
      stamp(px, py, thick, 1.0);
    }
  }

  // Fronds. A palm frond is a spine with leaflets hanging off both sides of
  // it, packed tightly enough near the spine to read as one blade and
  // separating into fingers toward the tip. Drawn as five straight wedges
  // from an atan2 it came out as a set of triangles; drawn as a spine with a
  // leaflet every five pixels it came out as a fish bone. Leaflets overlap
  // along the spine, sweep BACK toward the crown, and are longest a third of
  // the way out — which is where a frond is widest.
  {
    const double frond_a[9] = {-2.90, -2.45, -2.05, -1.62, -1.15, -0.62, -0.10,
                               0.35, 0.80};
    const double len_of[9]  = {0.62, 0.80, 0.95, 1.00, 0.97, 0.92, 0.85, 0.72, 0.58};
    const double reach = 0.205 * W;
    for (int fnd = 0; fnd < 9; ++fnd) {
      const double a0 = frond_a[fnd];
      const double len = reach * len_of[fnd];
      // A frond that reaches out sideways droops hardest; one that points up
      // barely droops at all.
      const double droop = 0.30 + 0.85 * std::pow(std::cos(a0) * 0.5 + 0.5, 0.6);
      const int steps = std::max(8, (int)len);
      double prev_x = crown_x, prev_y = crown_y;
      for (int i2 = 0; i2 <= steps; ++i2) {
        const double u = (double)i2 / steps;
        const double px = crown_x + std::cos(a0) * len * u;
        const double py = crown_y + std::sin(a0) * len * u + droop * len * u * u * 0.85;
        // The spine, tapering.
        stamp(px, py, 1.9 * (1.0 - 0.70 * u), 1.0);
        // Direction of travel, for leaflets to lie back against.
        const double tx = px - prev_x, ty = py - prev_y;
        const double tl = std::max(1e-6, std::hypot(tx, ty));
        const double ux = tx / tl, uy = ty / tl;
        prev_x = px; prev_y = py;
        if (u < 0.05) continue;
        // Widest a third of the way out, gone by the tip.
        // Serrated along its length: a frond's edge is a row of separate
        // leaflets, and a smooth outline reads as a leaf, not a palm.
        const double serr = 0.90 + 0.10 * std::sin(u * 3.14159 * 64.0);
        const double blade = (0.032 * W) * len_of[fnd] * serr *
                             std::pow(std::sin(3.14159 * std::pow(u, 0.72)), 0.85);
        for (int side = -1; side <= 1; side += 2) {
          // Perpendicular, then swept back along the spine by 40 degrees.
          const double nx = (-uy * side) * 0.77 - ux * 0.64;
          const double ny = ( ux * side) * 0.77 - uy * 0.64;
          const int ls = std::max(3, (int)blade);
          for (int j2 = 0; j2 <= ls; ++j2) {
            const double v = (double)j2 / ls;
            const double sag = 0.22 * blade * v * v;      // leaflets hang
            stamp(px + nx * blade * v, py + ny * blade * v + sag,
                  1.15 * (1.0 - 0.45 * v), 1.0);
          }
        }
      }
    }
    // The nuts, in the crook where the fronds leave the trunk.
    for (int n = 0; n < 4; ++n) {
      const double a = 0.9 + n * 0.55;
      stamp(crown_x + std::cos(a) * 0.012 * W, crown_y + 0.010 * H + std::sin(a) * 0.008 * H,
            0.0085 * W, 1.0);
    }
  }
}

static void draw_beach_with_moon(std::vector<uint32_t>& buf, int W, int H, CanvasState& st) {
  const size_t n = (size_t)W * (size_t)H;
  if (st.beach_w != W || st.beach_h != H || st.beach_still.size() != n) {
    st.beach_still.assign(n, 0xFF050B14);
    render_beach_still(st.beach_still, W, H);
    st.beach_w = W; st.beach_h = H;
  }
  std::memcpy(buf.data(), st.beach_still.data(), n * sizeof(uint32_t));
}

static void draw_cyber_mind_matrix(std::vector<uint32_t>& buf, int W, int H, uint64_t now_ms, CanvasState& st) {
  if (buf.size() != static_cast<size_t>(W) * static_cast<size_t>(H)) {
    buf.assign(static_cast<size_t>(W) * static_cast<size_t>(H), 0xFF040814);
  }

  const double t = now_ms * 0.001;
  const double cx = W * 0.5;
  const double cy = H * 0.42;

  for (int y = 0; y < H; ++y) {
    double ny = static_cast<double>(y) / H;
    int bg_r = static_cast<int>(4 + ny * 12);
    int bg_g = static_cast<int>(8 + ny * 20);
    int bg_b = static_cast<int>(20 + ny * 35);

    if (y > H * 0.58) {
      double gy = (y - H * 0.58) / (H * 0.42);
      double z = 1.0 / (gy + 0.05);
      double grid_z = std::fmod(z + t * 2.0, 1.0);
      if (grid_z < 0.05) {
        bg_r = std::min(255, bg_r + static_cast<int>(50 * gy));
        bg_g = std::min(255, bg_g + static_cast<int>(120 * gy));
        bg_b = std::min(255, bg_b + static_cast<int>(180 * gy));
      }
    }

    uint32_t row_pixel = 0xFF000000u | (bg_r << 16) | (bg_g << 8) | bg_b;
    for (int x = 0; x < W; ++x) {
      buf[static_cast<size_t>(y) * W + x] = row_pixel;
    }
  }

  if (H * 0.58 < H) {
    int y_start = static_cast<int>(H * 0.58);
    for (int i = -15; i <= 15; ++i) {
      double slope = i * 0.12;
      for (int y = y_start; y < H; ++y) {
        double gy = (y - y_start) / static_cast<double>(H - y_start);
        int x = static_cast<int>(cx + slope * W * gy);
        if (x >= 0 && x < W) {
          uint32_t cur = buf[static_cast<size_t>(y) * W + x];
          int r = std::min(255, static_cast<int>((cur >> 16) & 0xFF) + static_cast<int>(20 * gy));
          int g = std::min(255, static_cast<int>((cur >> 8) & 0xFF) + static_cast<int>(60 * gy));
          int b = std::min(255, static_cast<int>(cur & 0xFF) + static_cast<int>(100 * gy));
          buf[static_cast<size_t>(y) * W + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
      }
    }
  }

  // The core and its halo are pure functions of the squared distance from the
  // centre — nothing in them moves with the clock — so the per-channel adds
  // are tabulated once on d² (16 square-pixels per step, a tenth of a pixel of
  // radius at the core's edge) and each frame does a shift and three saturating
  // adds per pixel instead of a square root and two divisions.
  const double core_r = 75.0;
  constexpr int kCoreReach = 120;
  constexpr int kCoreShift = 4;
  constexpr size_t kCoreLutN = (2 * kCoreReach * kCoreReach) / (1 << kCoreShift) + 2;
  if (st.cyber_core_lut.size() != kCoreLutN) {
    st.cyber_core_lut.resize(kCoreLutN);
    for (size_t i = 0; i < kCoreLutN; ++i) {
      const double dist = std::sqrt((static_cast<double>(i) + 0.5) * (1 << kCoreShift));
      int ar = 0, ag = 0, ab = 0;
      if (dist <= core_r) {
        double factor = 1.0 - dist / core_r;
        ar = static_cast<int>(factor * 200);
        ag = static_cast<int>(factor * 240);
        ab = static_cast<int>(factor * 255);
      } else if (dist <= core_r * 2.5) {
        double glow = 1.0 - (dist - core_r) / (core_r * 1.5);
        glow = glow * glow;
        ar = static_cast<int>(glow * 70);
        ag = static_cast<int>(glow * 140);
        ab = static_cast<int>(glow * 200);
      }
      st.cyber_core_lut[i] = (static_cast<uint32_t>(ar) << 16) | (static_cast<uint32_t>(ag) << 8) | static_cast<uint32_t>(ab);
    }
  }
  for (int dy = -kCoreReach; dy <= kCoreReach; ++dy) {
    int y = static_cast<int>(cy + dy);
    if (y < 0 || y >= H) continue;
    uint32_t* row = &buf[static_cast<size_t>(y) * W];
    for (int dx = -kCoreReach; dx <= kCoreReach; ++dx) {
      int x = static_cast<int>(cx + dx);
      if (x < 0 || x >= W) continue;
      const uint32_t add = st.cyber_core_lut[static_cast<size_t>(dx * dx + dy * dy) >> kCoreShift];
      if (!add) continue;
      const uint32_t cur = row[x];
      int r = std::min(255, static_cast<int>((cur >> 16) & 0xFF) + static_cast<int>((add >> 16) & 0xFF));
      int g = std::min(255, static_cast<int>((cur >> 8) & 0xFF) + static_cast<int>((add >> 8) & 0xFF));
      int b = std::min(255, static_cast<int>(cur & 0xFF) + static_cast<int>(add & 0xFF));
      row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
  }

  const int num_nodes = 12;
  struct Node { double x, y; };
  Node nodes[num_nodes];
  for (int i = 0; i < num_nodes; ++i) {
    double angle = i * (2.0 * 3.14159265 / num_nodes) + t * 0.4;
    double rad = 220.0 + 30.0 * std::sin(i * 1.5 + t);
    nodes[i].x = cx + rad * std::cos(angle);
    nodes[i].y = cy + rad * 0.45 * std::sin(angle) + 20.0 * std::sin(i + t * 1.2);
  }

  for (int i = 0; i < num_nodes; ++i) {
    int next = (i + 1) % num_nodes;
    int next2 = (i + 3) % num_nodes;
    auto draw_beam = [&](int n1, int n2) {
      double x0 = nodes[n1].x, y0 = nodes[n1].y;
      double x1 = nodes[n2].x, y1 = nodes[n2].y;
      int steps = static_cast<int>(std::hypot(x1 - x0, y1 - y0));
      for (int step = 0; step < steps; ++step) {
        double frac = static_cast<double>(step) / std::max(1, steps);
        int px = static_cast<int>(x0 + (x1 - x0) * frac);
        int py = static_cast<int>(y0 + (y1 - y0) * frac);
        if (px >= 0 && px < W && py >= 0 && py < H) {
          uint32_t cur = buf[static_cast<size_t>(py) * W + px];
          int r = std::min(255, static_cast<int>(((cur >> 16) & 0xFF) + 30));
          int g = std::min(255, static_cast<int>(((cur >> 8) & 0xFF) + 100));
          int b = std::min(255, static_cast<int>((cur & 0xFF) + 160));
          buf[static_cast<size_t>(py) * W + px] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
      }
    };
    draw_beam(i, next);
    if (i % 2 == 0) draw_beam(i, next2);
  }

  for (int i = 0; i < num_nodes; ++i) {
    int nx = static_cast<int>(nodes[i].x);
    int ny = static_cast<int>(nodes[i].y);
    int nr = 8;
    for (int dy = -nr; dy <= nr; ++dy) {
      for (int dx = -nr; dx <= nr; ++dx) {
        int px = nx + dx, py = ny + dy;
        if (px >= 0 && px < W && py >= 0 && py < H && dx * dx + dy * dy <= nr * nr) {
          buf[static_cast<size_t>(py) * W + px] = 0xFF38BDF8;
        }
      }
    }
  }
}

static constexpr int kOrbPlasmaN = 8192;   // steps across the plasma's only range
static constexpr int kOrbGlowShift = 6;    // square-pixels per halo table step

static void draw_quantum_orb(std::vector<uint32_t>& buf, int W, int H, uint64_t now_ms, CanvasState& st) {
  const uint32_t kVoid = 0xFF03050C;
  if (buf.size() != static_cast<size_t>(W) * static_cast<size_t>(H)) {
    buf.assign(static_cast<size_t>(W) * static_cast<size_t>(H), kVoid);
  }

  const double t = now_ms * 0.0015;
  const double cx = W * 0.5;
  const double cy = H * 0.45;
  const double orb_r = 130.0;
  const double reach = orb_r * 3.0;            // past this every pixel is the void
  const double reach2 = reach * reach, orb_r2 = orb_r * orb_r;

  // The orb touches a disc 780px across. The rest of the display is one colour,
  // and used to be arrived at through a square root per pixel — two million of
  // them for a 1920x1080 frame. Fill the void flat, then paint only the box the
  // orb can reach.
  std::fill(buf.begin(), buf.end(), kVoid);

  // Colour, tabulated. Inside the orb the pixel is a function of one number —
  // the plasma value, the sum of three unit sinusoids, so it lives in [-3, 3];
  // 8192 steps put each step at 0.0007 of a radian, across which the fastest
  // channel moves 0.04 of a level. Outside it the halo is a function of the
  // squared distance alone. Neither depends on the clock, so both are built
  // once and every later frame is two array reads per pixel instead of six
  // transcendentals and a square root.
  if (st.orb_plasma_lut.size() != static_cast<size_t>(kOrbPlasmaN)) {
    st.orb_plasma_lut.resize(kOrbPlasmaN);
    for (int i = 0; i < kOrbPlasmaN; ++i) {
      const double p = -3.0 + 6.0 * i / (kOrbPlasmaN - 1);
      const int r = static_cast<int>(120 + 100 * std::sin(p));
      const int g = static_cast<int>(60 + 60 * std::cos(p + 2.0));
      const int b = static_cast<int>(200 + 55 * std::sin(p + 4.0));
      st.orb_plasma_lut[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
  }
  const size_t glow_n = static_cast<size_t>((reach2 - orb_r2)) >> kOrbGlowShift;
  if (st.orb_glow_lut.size() != glow_n + 2) {
    st.orb_glow_lut.assign(glow_n + 2, 0xFF000000u);
    for (size_t i = 0; i < glow_n + 2; ++i) {
      const double d2 = orb_r2 + (static_cast<double>(i) + 0.5) * (1 << kOrbGlowShift);
      double glow = 1.0 - (std::sqrt(d2) - orb_r) / (orb_r * 2.0);
      if (glow < 0.0) glow = 0.0;
      glow *= glow;
      const int r = static_cast<int>(glow * 140);
      const int g = static_cast<int>(glow * 70);
      const int b = static_cast<int>(glow * 220);
      st.orb_glow_lut[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
  }

  const int x0 = std::max(0, static_cast<int>(cx - reach) - 1);
  const int x1 = std::min(W, static_cast<int>(cx + reach) + 2);
  const int y0 = std::max(0, static_cast<int>(cy - reach) - 1);
  const int y1 = std::min(H, static_cast<int>(cy + reach) + 2);
  if (x1 <= x0 || y1 <= y0) return;

  // The three sinusoids the plasma is made of depend on x alone, on y alone,
  // and on x+y — so each is a small table for the whole frame rather than a
  // call per pixel. These are exact: the same value the call returned.
  std::vector<double> sin_x(static_cast<size_t>(x1 - x0));
  for (int x = x0; x < x1; ++x)
    sin_x[static_cast<size_t>(x - x0)] = std::sin(((x - cx) / orb_r) * 8.0 + t * 2.0);
  const int sum_base = x0 + y0;
  std::vector<double> sin_sum(static_cast<size_t>((x1 - x0) + (y1 - y0)));
  for (size_t i = 0; i < sin_sum.size(); ++i) {
    const double dsum = static_cast<double>(sum_base + static_cast<int>(i)) - cx - cy;
    sin_sum[i] = std::sin((dsum / orb_r) * 5.0 + t);
  }

  const double plasma_scale = (kOrbPlasmaN - 1) / 6.0;
  const int glow_last = static_cast<int>(st.orb_glow_lut.size()) - 1;

  for (int y = y0; y < y1; ++y) {
    const double dy = y - cy, dy2 = dy * dy;
    if (dy2 > reach2) continue;
    // Walk only the part of the row the disc actually covers. The corners of
    // the bounding box are a fifth of it and were being tested pixel by pixel.
    const double half = std::sqrt(reach2 - dy2);
    const int rx0 = std::max(x0, static_cast<int>(cx - half));
    const int rx1 = std::min(x1, static_cast<int>(cx + half) + 2);
    const double cos_y = std::cos((dy / orb_r) * 8.0 - t * 1.5);
    uint32_t* row = &buf[static_cast<size_t>(y) * static_cast<size_t>(W)];
    for (int x = rx0; x < rx1; ++x) {
      const double dx = x - cx;
      const double d2 = dx * dx + dy2;
      if (d2 > reach2) continue;

      if (d2 <= orb_r2) {
        const double plasma = sin_x[static_cast<size_t>(x - x0)] + cos_y +
                              sin_sum[static_cast<size_t>((x + y) - sum_base)];
        int idx = static_cast<int>((plasma + 3.0) * plasma_scale + 0.5);
        if (idx < 0) idx = 0;
        else if (idx >= kOrbPlasmaN) idx = kOrbPlasmaN - 1;
        row[x] = st.orb_plasma_lut[static_cast<size_t>(idx)];
      } else {
        int gi = static_cast<int>(d2 - orb_r2) >> kOrbGlowShift;
        if (gi < 0) gi = 0;
        else if (gi > glow_last) gi = glow_last;
        row[x] = st.orb_glow_lut[static_cast<size_t>(gi)];
      }
    }
  }
}

// The strings the start page's chrome is BUILT from, so the rect and the label
// can never come from two different places.
// This machine is not a hypervisor and there is no version 2.4 of it. It is
// the name of the thing, which is what a machine's own face should say.
const char* const kBrandLabel = "AIOS";
// The control opens the power panel — Restart, Power off, Sleep — the same one
// the workspace opens. It is not a restart button any more, so it does not say
// "RESTART", which on one stray click is what it used to be.
const char* const kRestartLabel = "POWER";
const char* const kAskLabel = "Ask Mind";
// A pill with a status dot: 8px to the dot, an 8px dot, 6px to the ink — then
// the same trailing gap the engine tag beside it uses.
constexpr int kDotInk = 22, kPillTrail = 12;
// A plain button: the same padding on both sides of the measured label.
constexpr int kBtnPad = 16;

std::string machine_conf_value(Kernel& k, const std::string& key) {
  std::string path;
  if (const char* c = std::getenv("AOS_CONF")) path = c;
  if (path.empty()) {
    const char* home = std::getenv("HOME");
    path = std::string(home ? home : ".") + "/.config/aos.conf";
  }
  auto file = k.host().read_file(path);
  if (!file.ok()) return {};
  const std::string body((const char*)file->data(), file->size());
  size_t at = 0;
  while (at < body.size()) {
    const size_t nl = body.find('\n', at);
    const std::string line = body.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
    at = (nl == std::string::npos) ? body.size() : nl + 1;
    if (line.rfind(key + "=", 0) != 0) continue;
    std::string v = line.substr(key.size() + 1);
    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
    return v;
  }
  return {};
}

MindSelect mind_select(HardwareCompositor& S, Kernel& k, const CanvasState& st,
                       int box_x, int box_w, int row_y) {
  MindSelect m;
  m.h = 28;
  m.y = row_y;
  m.row_h = S.line_height(Face::UiSmall) + 10;
  m.provider = machine_conf_value(k, "AOS_PROVIDER");
  m.model = machine_conf_value(k, "AOS_MODEL");
  if (m.provider.empty()) m.provider = "(none)";
  if (m.model.empty()) m.model = "(default)";

  const int pad = 14, arrow = 18, gap = 10, label_gap = 8;
  const int plab = S.label_width("PROVIDER", 1), mlab = S.label_width("MODEL", 1);
  m.prov_w = std::max(150, S.text_width(m.provider, Face::UiSmall) + pad * 2 + arrow);
  m.model_w = std::max(130, S.text_width(m.model, Face::UiSmall) + pad * 2 + arrow);

  const int total = plab + label_gap + m.prov_w + gap * 2 + mlab + label_gap + m.model_w;
  int cur = box_x + (box_w - total) / 2;
  cur += plab + label_gap;
  m.prov_x = cur;
  cur += m.prov_w + gap * 2 + mlab + label_gap;
  m.model_x = cur;

  // Whichever list is open, and where it hangs.
  m.which = st.mind_select_open;
  if (m.which == 1) {
    for (const MindPreset& p : st.mind_presets) {
      bool seen = false;
      for (const auto& h : m.list) if (h == p.provider) { seen = true; break; }
      if (!seen) m.list.push_back(p.provider);
    }
    if (m.list.empty()) m.list.push_back(m.provider);
    m.list_x = m.prov_x;
  } else if (m.which == 2) {
    m.list = st.mind_models;
    if (m.list.empty()) m.list.push_back(m.model);
    m.list_x = m.model_x;
  }
  if (m.which) {
    m.list_w = (m.which == 1) ? m.prov_w : m.model_w;
    for (const auto& o : m.list)
      m.list_w = std::max(m.list_w, S.text_width(o, Face::UiSmall) + pad * 2);
    m.list_y = m.y + m.h + 4;
  }
  return m;
}

CanvasChrome canvas_chrome(HardwareCompositor& S, int iw) {
  CanvasChrome c;
  // The room the SHARED top bar occupies, which this page lays itself out
  // under. It is drawn by the compositor at y=16 and is 48 tall, so the page
  // starts clear of 64 — it used to be a flat 40, which was the height of the
  // bar this page drew for itself and is eight pixels inside the one that
  // replaced it.
  c.topbar_h = 72;
  c.brand_x = 16;
  c.brand_y = 7;
  c.brand_h = 26;
  c.brand_w = kDotInk + S.text_width(kBrandLabel, Face::UiBold) + kPillTrail;
  c.rst_h = 26;
  c.rst_w = kDotInk + S.text_width(kRestartLabel, Face::UiBold) + kPillTrail;
  c.rst_x = iw - c.rst_w - 16;
  c.rst_y = 7;
  c.ask_h = 34;
  c.ask_w = S.text_width(kAskLabel, Face::UiBold) + 2 * kBtnPad;
  // The bar's centre used to be held clear of a hardcoded 410 — the right edge
  // of an old tagline pill that is no longer drawn. It is the brand
  // pill's own measured edge, plus a gap.
  c.left_edge = c.brand_right() + 16;
  return c;
}

// ---------------------------------------------------------------------------
// The desk
// ---------------------------------------------------------------------------
namespace {
// Both faces of the canvas used to be nailed down. The start page drew a
// stack-local array of six cards into a fixed 3x2 grid; the panel grid placed
// its seven panels from a per-column-count if/else. Neither could be picked
// up, folded away or thrown out, and the compositor hit-tested the cards by
// re-deriving the same numbers by hand — which is also why every click on a
// 4K display landed in the wrong place, the paint being at half scale there
// and the hit test at full.
//
// Now there is ONE list of tiles and ONE function that says where they are.

// How tall a folded tile is, on either face.
constexpr int kFoldH = 36;
// The fold and remove controls, and the corner a person resizes by.
constexpr int kTileCtl = 14;
constexpr int kTileGrip = 12;
// How much of a tile's top-right corner the controls own. Anything a tile
// right-aligns has to stop here or it is drawn straight over the x.
constexpr int kTileRight = 16 + 2 * (kTileCtl + 6);
// How far the pointer must travel before a press becomes a drag rather than
// the launch click the six cards have always been.
constexpr int kDragSlop = 16;

struct TileFace {
  TileKind kind;
  const char* slug;
  const char* menu;
  bool start_page;
};

// Every tile this desk knows how to draw. The slug is what a saved layout
// names it by, so these strings are load-bearing: renaming one orphans a
// person's arrangement.
const TileFace kTileFaces[] = {
  {TileKind::HeroEditor,   "code-editor",   "Code Editor",      true},
  {TileKind::HeroDb,       "db-studio",     "DB Studio",        true},
  {TileKind::HeroWeb,      "unblink-web",   "Unblink Web",      true},
  {TileKind::HeroTensor,   "tensor-hal",    "Tensor HAL",       true},
  {TileKind::HeroHorizon,  "horizon-vector","Horizon Vector",   true},
  {TileKind::HeroBuild,    "auto-build",    "Auto Build",       true},
  {TileKind::Telemetry,    "telemetry",     "Telemetry",        true},
  // The mind's own surfaces belong with the prompt you talk to it through:
  // what it is thinking, and what it made of the day. They were on the panel
  // grid, a face away from the only text field on the machine that addresses
  // it, so asking a question and watching it think were two different screens.
  {TileKind::ThoughtStream,"thought-stream","Thought Stream",   true},
  {TileKind::MindAnswer,   "mind-answer",   "What the Mind Said", true},
  {TileKind::Apps,         "apps",          "Applications",     true},
  {TileKind::ClockCalendar,"clock",         "Clock & Calendar", false},
  {TileKind::Performance,  "performance",   "Performance",      false},
  {TileKind::Identities,   "identities",    "Identities",       false},
  {TileKind::Authority,    "authority",     "Authority",        false},
  {TileKind::Directives,   "standing-orders","Standing Orders", false},
  {TileKind::Horizon,      "horizon",       "Horizon",          false},
  {TileKind::Brief,        "brief",         "The Brief",        true},
  {TileKind::Ledger,       "ledger",        "Authority Ledger", false},
  {TileKind::Rewind,       "rewind",        "Rewind",           false},
  {TileKind::AppPane,      "app",           "Application",      false},
  {TileKind::Inspect,      "inspect",       "Inspector",        false},
};

const TileFace& tile_face(TileKind kind) {
  for (const auto& f : kTileFaces) if (f.kind == kind) return f;
  return kTileFaces[0];
}

bool desk_fold_all() { return getenv("AOS_COLLAPSED_PANELS") != nullptr; }

// Is this tile drawn folded at the moment? The height it is ON SCREEN while
// folded is kFoldH -- its own title bar -- and that number must never be
// written back as the tile's size. See desk_unpin and desk_release: all three
// of them used to store whatever the grid had just given the tile, so a tile
// that was folded when it was moved or resized had 36 baked in as its OPEN
// height. Unfolding then produced a tile that is open and the height of its
// header, which is indistinguishable from one that never opened -- which is
// what happened to the mind's answer on the start page.
bool tile_shows_folded(const DeskTile& t) { return t.collapsed || desk_fold_all(); }

// Same run directory as the scene, so a machine that can be told what to show
// can also remember it. Hardcoded to one host, the save silently failed
// everywhere else and the desk went back to whatever it booted with.
std::string desk_layout_path(Kernel& k) { return run_dir(k) + "/desktop-layout"; }

std::string num(int v) {
  bool neg = v < 0;
  unsigned long long m = neg ? (unsigned long long)(-(long long)v) : (unsigned long long)v;
  std::string s = dec(m);
  return neg ? "-" + s : s;
}

}  // namespace

// Where the panel grid's face begins, and where the start page's is, in the
// desk's own terms — read from the same env the painter has always read. At
// file scope because the compositor has to be able to say which VIEW is up
// without laying the whole canvas out to find out.
bool desk_start_page(const CanvasState& st) {
  if (getenv("AOS_SHOW_PANELS") != nullptr) return false;
  if (st.bare_scene) return false;
  return st.hide_panels || getenv("AOS_HIDE_PANELS") != nullptr || getenv("AOS_NO_PANELS") != nullptr;
}

bool tile_is_start_page(TileKind kind) { return tile_face(kind).start_page; }
const char* tile_slug(TileKind kind) { return tile_face(kind).slug; }
const char* tile_menu_name(TileKind kind) { return tile_face(kind).menu; }

void seed_desk_tiles(CanvasState& st) {
  // Exactly the two arrangements the hardcoded painters drew, said in data:
  // three launcher cards over three more, then the two full-width bands; and
  // the panel grid's three columns of clock/performance/identities,
  // authority/orders and horizon/brief.
  struct Seed { TileKind kind; int col, slot, span, height; };
  static const Seed kSeeds[] = {
    // The six launcher cards are NOT seeded any more. An editor, a SQL client
    // and a browser written from scratch inside this OS invite comparison with
    // software that has a thousand engineer-years in it, and they were the FIRST
    // thing on the machine's own face while the things only this machine can do
    // were hidden behind an unlabelled key. The panes are still there — the top
    // bar and F4/F5/F6 raise them.
    {TileKind::Telemetry,     0, 0, 3,  72},
    // Directly under the prompt on the landing face, because that is where the
    // question was typed. A band across all three columns: an answer is prose,
    // and prose reads across a width.
    {TileKind::MindAnswer,    0, 2, 3, -46},
    // The launcher, beside the answer. Applications were reachable only by
    // typing `app open <name>` into the shell pane, which is not a launcher.
    {TileKind::Apps,          0, 4, 1, 200},
    // The desk, in the order a person who runs this machine would read it: what
    // the mind decided and why, then what it did on whose authority, then how to
    // put it back, then the standing orders nobody typed, then the world.
    // Negative is a percentage of the body; 0 is "whatever is left".
    // A band is always laid out UNDER the columns, which is where the brief
    // wants to be anyway: it is prose, and prose reads across a width, not down
    // a third of one.
    {TileKind::Brief,         0, 8, 3, -28},
    {TileKind::Ledger,        0, 0, 1,   0},
    {TileKind::Identities,    0, 2, 1, -18},
    {TileKind::Authority,     1, 0, 1,   0},
    {TileKind::Rewind,        1, 2, 1, 152},
    {TileKind::Horizon,       2, 0, 1, -34},
    {TileKind::Directives,    2, 2, 1, 116},
    {TileKind::ThoughtStream, 2, 4, 1,   0},
    // Kept, not seeded onto the glass: the header already says how long this
    // machine has been up and the workspace's own bar carries a clock. The
    // context menu puts either back.
    {TileKind::ClockCalendar, 0, 6, 1, 160},
    {TileKind::Performance,   0, 8, 1, 130},
    // The application pane, which is not on the glass until something is
    // opened. It never joins the grid: an app is a window-shaped thing and the
    // person who opened it put it where they put it.
    {TileKind::AppPane,       0, 0, 1,   0},
    {TileKind::Inspect,       0, 0, 1,   0},
  };
  st.tiles.clear();
  st.tiles.reserve(sizeof(kSeeds) / sizeof(kSeeds[0]));
  for (const Seed& s : kSeeds) {
    DeskTile t;
    t.kind = s.kind; t.col = s.col; t.slot = s.slot; t.span = s.span; t.height = s.height;
    if (s.kind == TileKind::ClockCalendar || s.kind == TileKind::Performance) t.hidden = true;
    if (s.kind == TileKind::AppPane || s.kind == TileKind::Inspect) { t.hidden = true; t.floating = true; }
    st.tiles.push_back(t);
  }
  st.tiles_seeded = true;
}

std::string desk_layout_text(const CanvasState& st) {
  std::string out = "# aios desktop layout: tile <name> <col> <slot> <span> <height> <flags> <x> <y> <w> <h>\n";
  // A background belongs to the view it is behind, so it is saved beside the
  // tiles rather than in the run directory's one-word "show this" file.
  // Whether the person asked for the load figures. A preference, so it comes
  // back the way they left it.
  out += std::string("loadmeter ") + (st.show_machine_load ? "1" : "0") + "\n";
  // The hour the shoreline was left at, in thousandths — the layout file is
  // whitespace-separated words and an integer is one word in every locale.
  out += "beachtod " + num((int)(st.scene_tod * 1000.0f + 0.5f)) + "\n";
  out += "scene start " + num(st.scene(DeskView::StartPage)) + "\n";
  out += "scene desk " + num(st.scene(DeskView::Desk)) + "\n";
  out += "scene workspace " + num(st.scene(DeskView::Workspace)) + "\n";
  // One line per windowed section, so a desktop comes back up on the wallpaper
  // it was left on.
  for (int i = 0; i < CanvasState::kSections; ++i)
    out += "section " + num(i) + " " + num(st.section_scene[i]) + "\n";
  for (const DeskTile& t : st.tiles) {
    // The application pane is saved WHERE it is but never as open. Nothing is
    // loaded in it after a restart, so a machine that came back up with the
    // tile still on the glass would be showing an empty window and calling it
    // an app. Opening something puts it back, at the size and place it had.
    const bool open_now = (t.kind == TileKind::AppPane || t.kind == TileKind::Inspect) ? false : !t.hidden;
    const int flags = (t.collapsed ? 1 : 0) | (open_now ? 0 : 2) | (t.floating ? 4 : 0);
    out += "tile ";
    out += tile_slug(t.kind);
    out += " " + num(t.col) + " " + num(t.slot) + " " + num(t.span) + " " + num(t.height) +
           " " + num(flags) + " " + num(t.x) + " " + num(t.y) + " " + num(t.w) + " " + num(t.h) + "\n";
  }
  return out;
}

void desk_layout_save(CanvasState& st, Kernel& k) {
  if (!st.layout_dirty) return;
  // Never before it has been READ. A view's background can be changed from the
  // workspace, where this painter never runs — and saving a desk that is still
  // the seeded one would throw away an arrangement nobody has looked at yet.
  if (!st.layout_loaded) return;
  st.layout_dirty = false;
  const std::string text = desk_layout_text(st);
  (void)k.host().write_file(desk_layout_path(k),
                            ByteSpan(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

void desk_layout_parse(CanvasState& st, const std::string& text) {
  if (!st.tiles_seeded) seed_desk_tiles(st);
  size_t at = 0;
  while (at < text.size()) {
    size_t eol = text.find('\n', at);
    std::string line = text.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
    at = (eol == std::string::npos) ? text.size() : eol + 1;
    // Split on runs of blanks; a line that is not a tile line is skipped, so a
    // comment, a blank, or a field this version does not know costs nothing.
    std::vector<std::string> f;
    for (size_t p = 0; p < line.size();) {
      while (p < line.size() && (line[p] == ' ' || line[p] == '\t' || line[p] == '\r')) ++p;
      size_t e = p;
      while (e < line.size() && line[e] != ' ' && line[e] != '\t' && line[e] != '\r') ++e;
      if (e > p) f.push_back(line.substr(p, e - p));
      p = e;
    }
    if (f.size() >= 3 && f[0] == "scene") {
      int v = 0;
      for (char c : f[2]) { if (c < '0' || c > '9') { v = -1; break; } v = v * 10 + (c - '0'); }
      if (v < 0 || v > 4) continue;
      if (f[1] == "start") st.set_scene(DeskView::StartPage, v);
      else if (f[1] == "desk") st.set_scene(DeskView::Desk, v);
      else if (f[1] == "workspace") st.set_scene(DeskView::Workspace, v);
      continue;
    }
    if (f.size() >= 2 && f[0] == "beachtod") {
      int v = 0; bool ok = !f[1].empty();
      for (char c : f[1]) { if (c < '0' || c > '9') { ok = false; break; } v = v * 10 + (c - '0'); }
      if (ok && v >= 0 && v <= 1000) st.scene_tod = (float)v / 1000.0f;
      continue;
    }
    if (f.size() >= 2 && f[0] == "loadmeter") {
      st.show_machine_load = (f[1] == "1");
      continue;
    }
    if (f.size() >= 3 && f[0] == "section") {
      int idx = 0, val = 0;
      bool ok = !f[1].empty() && !f[2].empty();
      for (char c : f[1]) { if (c < '0' || c > '9') { ok = false; break; } idx = idx * 10 + (c - '0'); }
      for (char c : f[2]) { if (c < '0' || c > '9') { ok = false; break; } val = val * 10 + (c - '0'); }
      if (ok && idx >= 0 && idx < CanvasState::kSections && val >= 0 && val <= 4)
        st.section_scene[idx] = val;
      continue;
    }
    if (f.size() < 7 || f[0] != "tile") continue;
    DeskTile* into = nullptr;
    for (DeskTile& t : st.tiles) if (f[1] == tile_slug(t.kind)) { into = &t; break; }
    if (!into) continue;   // a tile this build does not have: the file outlives it
    auto ival = [&](size_t i, int fallback) {
      if (i >= f.size()) return fallback;
      const std::string& s = f[i];
      size_t p = (s[0] == '-') ? 1 : 0;
      if (p >= s.size()) return fallback;
      long v = 0;
      for (size_t q = p; q < s.size(); ++q) {
        if (s[q] < '0' || s[q] > '9') return fallback;
        v = v * 10 + (s[q] - '0');
        if (v > 100000) return fallback;
      }
      return (int)(s[0] == '-' ? -v : v);
    };
    into->col = std::clamp(ival(2, into->col), 0, 2);
    into->slot = std::clamp(ival(3, into->slot), 0, 4096);
    into->span = std::clamp(ival(4, into->span), 1, 3);
    into->height = std::clamp(ival(5, into->height), -100, 20000);
    const int flags = ival(6, 0);
    into->collapsed = (flags & 1) != 0;
    into->hidden = (flags & 2) != 0;
    into->floating = (flags & 4) != 0;
    into->x = ival(7, into->x);
    into->y = ival(8, into->y);
    into->w = ival(9, into->w);
    into->h = ival(10, into->h);
    // A height of the fold height on a tile that is NOT folded is a layout
    // written by the bug above, and it survives a rebuild: the file is read
    // once and saved every frame, so the machine would keep handing itself the
    // same 36px sliver forever. Zero means "the default", which is what a tile
    // that has never been resized carries.
    if (!into->collapsed && into->h > 0 && into->h <= kFoldH) { into->h = 0; st.layout_dirty = true; }
    if (!into->collapsed && into->height > 0 && into->height <= kFoldH) {
      into->height = 0;
      st.layout_dirty = true;
    }
  }
}

static void draw_composed_scene(std::vector<uint32_t>& buf, int W, int H, uint64_t now_ms, CanvasState& st) {
  if (buf.size() != (size_t)W * (size_t)H) {
    buf.assign((size_t)W * (size_t)H, 0xFF050B14);
  }

  SceneDescription desc;
  if (!st.composed_text.empty()) {
    auto parsed = SceneDescription::from_text(st.composed_text);
    if (parsed.ok()) desc = *parsed;
  }

  const double t = now_ms * 0.001;
  const int y_horizon = (int)(H * 0.52);

  // Extract sky colors
  const int sky_low_r = (int)(desc.sky_low[0] * 255.0f + 0.5f);
  const int sky_low_g = (int)(desc.sky_low[1] * 255.0f + 0.5f);
  const int sky_low_b = (int)(desc.sky_low[2] * 255.0f + 0.5f);

  const int sky_hi_r = (int)(desc.sky_high[0] * 255.0f + 0.5f);
  const int sky_hi_g = (int)(desc.sky_high[1] * 255.0f + 0.5f);
  const int sky_hi_b = (int)(desc.sky_high[2] * 255.0f + 0.5f);

  // Sun position from sun_dir
  const double sun_cx = W * (0.5 + 0.38 * desc.sun_dir[0]);
  const double sun_cy = y_horizon - std::max(20.0, (double)H * 0.35 * std::max(0.08f, desc.sun_dir[1]));
  const double sun_r = 28.0;

  const int sun_r_val = (int)(desc.sun_colour[0] * 255.0f * desc.sun_intensity + 0.5f);
  const int sun_g_val = (int)(desc.sun_colour[1] * 255.0f * desc.sun_intensity + 0.5f);
  const int sun_b_val = (int)(desc.sun_colour[2] * 255.0f * desc.sun_intensity + 0.5f);

  // Ground colors
  const int gnd_r = (int)(desc.ground_colour[0] * 255.0f + 0.5f);
  const int gnd_g = (int)(desc.ground_colour[1] * 255.0f + 0.5f);
  const int gnd_b = (int)(desc.ground_colour[2] * 255.0f + 0.5f);

  // Render scanlines
  for (int y = 0; y < H; ++y) {
    uint32_t* row = &buf[(size_t)y * W];
    if (y < y_horizon) {
      // Sky gradient
      double v = (double)y / std::max(1, y_horizon);
      int r = std::clamp((int)(sky_hi_r + v * (sky_low_r - sky_hi_r)), 0, 255);
      int g = std::clamp((int)(sky_hi_g + v * (sky_low_g - sky_hi_g)), 0, 255);
      int b = std::clamp((int)(sky_hi_b + v * (sky_low_b - sky_hi_b)), 0, 255);

      for (int x = 0; x < W; ++x) {
        int pr = r, pg = g, pb = b;
        double dx = x - sun_cx;
        double dy = y - sun_cy;
        double dist2 = dx * dx + dy * dy;
        if (dist2 < sun_r * sun_r * 25.0) {
          double dist = std::sqrt(dist2);
          if (dist < sun_r) {
            double core_f = 1.0 - (dist / sun_r);
            pr = std::min(255, (int)(pr * (1.0 - core_f) + sun_r_val * core_f + 40));
            pg = std::min(255, (int)(pg * (1.0 - core_f) + sun_g_val * core_f + 20));
            pb = std::min(255, (int)(pb * (1.0 - core_f) + sun_b_val * core_f));
          } else {
            double glow_f = 1.0 - (dist - sun_r) / (sun_r * 4.0);
            if (glow_f > 0.0) {
              glow_f = glow_f * glow_f;
              pr = std::min(255, (int)(pr + glow_f * sun_r_val * 0.7));
              pg = std::min(255, (int)(pg + glow_f * sun_g_val * 0.5));
              pb = std::min(255, (int)(pb + glow_f * sun_b_val * 0.3));
            }
          }
        }
        row[x] = 0xFF000000u | (pr << 16) | (pg << 8) | pb;
      }
    } else {
      // Ground / Water
      double gy = (double)(y - y_horizon) / std::max(1, (H - y_horizon));
      if (desc.ground == GroundKind::Water) {
        for (int x = 0; x < W; ++x) {
          // Dynamic water surface
          int wr = std::clamp((int)(gnd_r + gy * 18), 0, 255);
          int wg = std::clamp((int)(gnd_g + gy * 32), 0, 255);
          int wb = std::clamp((int)(gnd_b + gy * 55), 0, 255);

          // Moving wave harmonics
          double wave = std::sin(x * 0.025 + y * 0.22 - t * 2.2) * std::cos(x * 0.015 - y * 0.1 + t * 1.4);
          if (wave > 0.25) {
            wr = std::min(255, wr + (int)(25 * wave * gy));
            wg = std::min(255, wg + (int)(45 * wave * gy));
            wb = std::min(255, wb + (int)(70 * wave * gy));
          }

          // Sun reflection column on moving water
          double dx_sun = std::abs(x - sun_cx);
          double spread = (y - y_horizon) * 0.42 + 18.0;
          if (dx_sun < spread) {
            double refl_shape = (1.0 - dx_sun / spread);
            double shimmer = 0.5 + 0.5 * std::sin(x * 0.09 + y * 0.45 - t * 3.5) * std::cos(x * 0.04 - y * 0.25 + t * 2.0);
            double refl = refl_shape * refl_shape * shimmer;
            wr = std::min(255, (int)(wr + refl * sun_r_val * 0.85));
            wg = std::min(255, (int)(wg + refl * sun_g_val * 0.65));
            wb = std::min(255, (int)(wb + refl * sun_b_val * 0.45));
          }

          // Shoreline foam near bottom
          if (y > H - 18) {
            double foam = (y - (H - 18)) / 18.0;
            double foam_noise = 0.5 + 0.5 * std::sin(x * 0.15 + t * 4.0);
            if (foam_noise > 0.3) {
              wr = std::min(255, (int)(wr + foam * 140));
              wg = std::min(255, (int)(wg + foam * 160));
              wb = std::min(255, (int)(wb + foam * 180));
            }
          }

          row[x] = 0xFF000000u | (wr << 16) | (wg << 8) | wb;
        }
      } else {
        // Plain / Desert / Rock terrain
        for (int x = 0; x < W; ++x) {
          int tr = std::clamp((int)(gnd_r + gy * 30), 0, 255);
          int tg = std::clamp((int)(gnd_g + gy * 22), 0, 255);
          int tb = std::clamp((int)(gnd_b + gy * 16), 0, 255);
          uint32_t sh = (uint32_t)(x * 1618033 + y * 3141592);
          int grain = ((sh >> 4) & 0xF) - 7;
          tr = std::clamp(tr + grain, 0, 255);
          tg = std::clamp(tg + grain, 0, 255);
          tb = std::clamp(tb + grain, 0, 255);
          row[x] = 0xFF000000u | (tr << 16) | (tg << 8) | tb;
        }
      }
    }
  }

  // Render any solids (pyramid, sphere, etc.)
  for (const auto& solid : desc.solids) {
    if (solid.kind == SolidKind::Pyramid) {
      double px = W * 0.5 + solid.x * (W * 0.15);
      double py = y_horizon + 15.0;
      double base_w = solid.sx * 140.0;
      double height = solid.sy * 160.0;
      int top_y = (int)(py - height);
      int bot_y = (int)py;
      for (int sy = std::max(0, top_y); sy <= std::min(H - 1, bot_y); ++sy) {
        double f = (double)(sy - top_y) / std::max(1.0, height);
        double hw = f * base_w * 0.5;
        int min_x = (int)(px - hw);
        int max_x = (int)(px + hw);
        for (int sx = std::max(0, min_x); sx <= std::min(W - 1, max_x); ++sx) {
          bool lit = (sx < px);
          int sr = (int)(solid.r * (lit ? 240 : 160));
          int sg = (int)(solid.g * (lit ? 180 : 120));
          int sb = (int)(solid.b * (lit ? 120 : 80));
          buf[(size_t)sy * W + sx] = 0xFF000000u | (sr << 16) | (sg << 8) | sb;
        }
      }
    }
  }
}

void paint_scene(std::vector<uint32_t>& out, int W, int H, uint64_t now_ms, CanvasState& st, int scene) {
  if (W <= 0 || H <= 0) return;
  if (out.size() != (size_t)W * (size_t)H) out.assign((size_t)W * (size_t)H, 0xFF050B14);
  if (scene == 1) draw_cyber_mind_matrix(out, W, H, now_ms, st);
  else if (scene == 2) draw_quantum_orb(out, W, H, now_ms, st);
  // No description loaded means there is nothing the PERSON asked for to draw,
  // and the painter does not get to fill that hole with something it made up.
  // These two lines invented a scene ("ocean at dusk sunset open horizon...",
  // "three pyramids in the desert...") and wrote it into live state, so a
  // machine coming back on a composed scene whose description had not been read
  // yet — the sidecar missing, half-written, or a cleaned run directory — put a
  // picture nobody chose on the glass and then carried it forward as if it had
  // been asked for. draw_composed_scene already renders an empty description
  // from its defaults; that is honest emptiness, not a guess at content. Who
  // owns what is drawn here is whoever asked for it.
  else if (scene == 4 || scene == 3) draw_composed_scene(out, W, H, now_ms, st);
  else draw_beach_with_moon(out, W, H, st);
}

CanvasLayout canvas_layout(HardwareCompositor& S, uint32_t W, uint32_t H, const CanvasState& st) {
  CanvasLayout L;
  // The SAME scale the painter picks. This is the whole reason the hit test
  // gets the layout from here: a 4K display paints at half scale, and a mouse
  // handler that worked in screen pixels was clicking twice as far into the
  // page as the person had aimed.
  L.scale = (W >= 3000 || H >= 1700) ? 2 : 1;
  L.iw = (int)(W / (uint32_t)L.scale);
  L.ih = (int)(H / (uint32_t)L.scale);
  L.start_page = desk_start_page(st);
  if (L.iw < 420 || L.ih < 300) return L;

  if (L.start_page) {
    const CanvasChrome ch = canvas_chrome(S, L.iw);
    const int box_w = std::min(1040, L.iw - 48);
    const int box_y = ch.topbar_h + 16 + 28 + 82;   // topbar, hero gap, emblem, drop to the bar
    L.cols = 3;
    L.gap = 14;
    L.col_gap = 14;
    L.body_x = (L.iw - box_w) / 2;
    L.body_w = box_w;
    L.body_y = box_y + 56 + 10 + 24 + 14;           // under the mind bar & theme chips bar
    L.body_h = L.ih - L.body_y - 34;                // clear of the bottom dock
  } else {
    L.cols = L.iw >= 1150 ? 3 : L.iw >= 780 ? 2 : 1;
    const int pad = L.cols == 3 ? 24 : 16;
    const bool lanes = L.ih >= 640 && L.iw >= 780;
    L.gap = pad / 2;
    L.col_gap = pad;
    L.body_x = pad;
    L.body_w = L.iw - pad * 2;
    L.body_y = 78 + pad;
    L.body_h = L.ih - 78 - (lanes ? 96 : 0) - pad * 2;
  }
  if (L.body_h < kFoldH || L.body_w < 100) { L.body_h = std::max(0, L.body_h); return L; }
  L.col_w = (L.body_w - L.col_gap * (L.cols - 1)) / L.cols;

  const bool fold_all = desk_fold_all();
  auto folded = [&](const DeskTile& t) { return t.collapsed || fold_all; };
  auto col_x = [&](int c) { return L.body_x + (L.col_w + L.col_gap) * c; };
  auto span_w = [&](int c, int span) {
    const int s = std::clamp(span, 1, L.cols - c);
    return L.col_w * s + L.col_gap * (s - 1);
  };
  auto rect_for = [&](const DeskTile& t, int idx, int x, int y, int w, int h) {
    TileRect r;
    r.kind = t.kind;
    r.index = idx;
    r.x = x; r.y = y;
    r.w = std::max(60, w);
    r.h = std::max(kFoldH, h);
    r.header_h = std::min(r.h, 26);
    r.ctl = (r.w >= 150) ? kTileCtl : 0;
    r.shut_x = r.x + r.w - 10 - r.ctl;
    r.shut_y = r.y + (folded(t) ? (r.h - r.ctl) / 2 : 7);
    r.fold_x = r.shut_x - r.ctl - 5;
    r.fold_y = r.shut_y;
    // A floating tile is a window, and a window is resized by its corner — so
    // that corner has to be big enough to aim at. 12px over a live page is a
    // target nobody finds.
    r.grip = (r.w >= 150 && r.h > kFoldH + 8) ? (t.floating ? kTileGrip + 8 : kTileGrip) : 0;
    r.floating = t.floating;
    return r;
  };

  // Split this face into its columns, its full-width bands, and whatever a
  // person has unpinned to float on top.
  std::vector<int> in_col[3];
  std::vector<int> bands, floats;
  for (size_t i = 0; i < st.tiles.size(); ++i) {
    const DeskTile& t = st.tiles[i];
    // The application pane is on whichever face is up — it is the one tile that
    // is not a start-page thing or a desk thing.
    if (t.hidden) continue;
    if (t.kind != TileKind::AppPane && t.kind != TileKind::Inspect &&
        tile_is_start_page(t.kind) != L.start_page) continue;
    if (t.floating) { floats.push_back((int)i); continue; }
    if (t.span >= L.cols) { bands.push_back((int)i); continue; }
    in_col[std::clamp(t.col, 0, L.cols - 1)].push_back((int)i);
  }
  // By slot, ties broken by position in the desk, which makes the order total
  // and lets this be a plain sort: stable_sort takes a temporary buffer out of
  // the allocator, and this runs inside every frame.
  auto by_slot = [&](int a, int b) {
    return st.tiles[a].slot != st.tiles[b].slot ? st.tiles[a].slot < st.tiles[b].slot : a < b;
  };
  for (int c = 0; c < L.cols; ++c) std::sort(in_col[c].begin(), in_col[c].end(), by_slot);
  std::sort(bands.begin(), bands.end(), by_slot);
  std::sort(floats.begin(), floats.end(), by_slot);

  // What a tile asks for: a fixed height, a share of the body, or "whatever is
  // left" — the three things both old layouts were already doing by hand.
  auto asked = [&](const DeskTile& t) {
    if (folded(t)) return kFoldH;
    if (t.height > 0) return t.height;
    if (t.height < 0) return std::max(kFoldH, L.body_h * (-t.height) / 100);
    return 0;
  };

  int band_fixed = 0, band_fillers = 0;
  for (int i : bands) { const int hgt = asked(st.tiles[i]); if (hgt) band_fixed += hgt; else ++band_fillers; }
  const int band_gaps = bands.empty() ? 0 : L.gap * (int)bands.size();

  int col_natural = 0;
  bool col_filler = false, any_col = false;
  for (int c = 0; c < L.cols; ++c) {
    if (in_col[c].empty()) continue;
    any_col = true;
    int nat = L.gap * ((int)in_col[c].size() - 1);
    for (int i : in_col[c]) { const int hgt = asked(st.tiles[i]); if (hgt) nat += hgt; else col_filler = true; }
    col_natural = std::max(col_natural, nat);
  }
  const int for_cols = std::max(kFoldH, L.body_h - band_fixed - band_gaps);
  int col_region = !any_col ? 0 : col_filler ? for_cols : std::min(col_natural, for_cols);

  for (int c = 0; c < L.cols; ++c) {
    int fixed = 0, fillers = 0;
    for (int i : in_col[c]) { const int hgt = asked(st.tiles[i]); if (hgt) fixed += hgt; else ++fillers; }
    const int gaps = in_col[c].empty() ? 0 : L.gap * ((int)in_col[c].size() - 1);
    const int share = fillers ? std::max(kFoldH, (col_region - gaps - fixed) / fillers) : 0;
    int y = L.body_y;
    for (int i : in_col[c]) {
      const DeskTile& t = st.tiles[i];
      int hgt = asked(t);
      if (!hgt) hgt = share;
      L.tiles.push_back(rect_for(t, i, col_x(c), y, span_w(c, t.span), hgt));
      y += hgt + L.gap;
    }
  }

  int band_y = L.body_y + col_region + (col_region ? L.gap : 0);
  const int band_room = L.body_h - col_region - (col_region ? L.gap : 0);
  const int band_share = band_fillers
      ? std::max(kFoldH, (band_room - band_fixed - L.gap * ((int)bands.size() - 1)) / band_fillers) : 0;
  for (int i : bands) {
    const DeskTile& t = st.tiles[i];
    int hgt = asked(t);
    if (!hgt) hgt = band_share;
    L.tiles.push_back(rect_for(t, i, L.body_x, band_y, L.body_w, hgt));
    band_y += hgt + L.gap;
  }

  // Unpinned tiles float over the grid, clamped so none of them can be pushed
  // off the glass and become unreachable.
  for (int i : floats) {
    const DeskTile& t = st.tiles[i];
    const int w = std::clamp(t.w > 0 ? t.w : L.col_w, 120, L.iw - 8);
    const int h = folded(t) ? kFoldH : std::clamp(t.h > 0 ? t.h : 140, kFoldH, L.ih - 8);
    L.tiles.push_back(rect_for(t, i, std::clamp(t.x, 0, L.iw - w), std::clamp(t.y, 0, L.ih - h), w, h));
  }

  // The tile in hand goes where the hand is, and draws over everything else.
  if (st.drag_tile >= 0 && st.drag_moved) {
    for (size_t n = 0; n < L.tiles.size(); ++n) {
      if (L.tiles[n].index != st.drag_tile) continue;
      TileRect r = L.tiles[n];
      if (st.drag_resize) {
        r.w = std::clamp(st.drag_from_w + (st.drag_at_x - st.drag_grab_x), 120, L.iw - r.x - 4);
        r.h = std::clamp(st.drag_from_h + (st.drag_at_y - st.drag_grab_y), kFoldH, L.ih - r.y - 4);
      } else {
        r.x = std::clamp(st.drag_at_x - st.drag_off_x, 0, std::max(0, L.iw - r.w));
        r.y = std::clamp(st.drag_at_y - st.drag_off_y, 0, std::max(0, L.ih - r.h));
      }
      r = rect_for(st.tiles[st.drag_tile], r.index, r.x, r.y, r.w, r.h);
      L.tiles.erase(L.tiles.begin() + (long)n);
      L.tiles.push_back(r);
      break;
    }
  }
  return L;
}

void desk_press(CanvasState& st, const TileRect& r, int px, int py, bool resize) {
  st.drag_tile = r.index;
  st.drag_resize = resize;
  st.drag_moved = false;
  st.drag_grab_x = px; st.drag_grab_y = py;
  st.drag_at_x = px;   st.drag_at_y = py;
  st.drag_off_x = px - r.x; st.drag_off_y = py - r.y;
  st.drag_from_w = r.w; st.drag_from_h = r.h;
}

void desk_drag_motion(CanvasState& st, int px, int py) {
  if (st.drag_tile < 0 || st.drag_tile >= (int)st.tiles.size()) return;
  st.drag_at_x = px;
  st.drag_at_y = py;
  // A press that has not travelled is still a press. The six launcher cards
  // are clicked far more often than they are moved, so the click wins until
  // the pointer has actually gone somewhere.
  if (!st.drag_moved &&
      (px - st.drag_grab_x > kDragSlop || st.drag_grab_x - px > kDragSlop ||
       py - st.drag_grab_y > kDragSlop || st.drag_grab_y - py > kDragSlop)) {
    st.drag_moved = true;
  }
  if (!st.drag_moved || st.drag_resize) return;
  // A tile being dragged goes where the hand takes it, full stop. This used to
  // work out which column and which gap the grid would swallow it into, so a
  // card dropped in the middle of the screen snapped back into a lane and the
  // desk could only ever be one of the arrangements the grid could express.
  // Dropping IS unpinning (desk_release), so there is no grid target to work
  // out and no lane to hint at: the tile under the hand is the whole preview.
}

int desk_release(CanvasState& st, const CanvasLayout& L) {
  const int idx = st.drag_tile;
  st.drag_tile = -1;
  if (idx < 0 || idx >= (int)st.tiles.size()) { st.drag_moved = false; st.drag_resize = false; return -1; }
  const bool moved = st.drag_moved, was_resize = st.drag_resize;
  st.drag_moved = false;
  st.drag_resize = false;
  DeskTile& t = st.tiles[idx];
  bool is_hero = (t.kind == TileKind::HeroEditor || t.kind == TileKind::HeroDb ||
                  t.kind == TileKind::HeroWeb || t.kind == TileKind::HeroTensor ||
                  t.kind == TileKind::HeroHorizon || t.kind == TileKind::HeroBuild);
  if (!moved || (is_hero && !was_resize)) return idx;   // a click, not a resize move: the card launches
  const int dx = st.drag_at_x - st.drag_grab_x, dy = st.drag_at_y - st.drag_grab_y;
  if (was_resize) {
    const int nw = std::clamp(st.drag_from_w + dx, 120, std::max(120, L.iw - 8));
    const int nh = std::clamp(st.drag_from_h + dy, kFoldH, std::max(kFoldH, L.ih - 8));
    if (t.floating) { t.w = nw; if (!tile_shows_folded(t)) t.h = nh; }
    else {
      // Widening a grid tile takes columns, not pixels: rounded to the nearest
      // whole column, and a tile as wide as the face becomes a band.
      const int step = L.col_w + L.col_gap;
      t.span = step > 0 ? std::clamp((nw + step / 2) / step, 1, L.cols) : 1;
      if (!tile_shows_folded(t)) t.height = nh;
    }
    st.layout_dirty = true;
    return -1;
  }
  // Wherever it was let go. A tile that was still in the grid leaves it here —
  // dragging something IS saying where you want it, and a desk that answers by
  // dropping the card into the nearest lane is a desk that overrules you. It
  // keeps the size and the place the grid had just given it, so unpinning moves
  // nothing but what happens next; the tile's own menu pins it back.
  t.x = st.drag_at_x - st.drag_off_x;
  t.y = st.drag_at_y - st.drag_off_y;
  if (!t.floating) {
    t.floating = true;
    // The size the grid had given it at the press, so it does not change shape
    // in the hand -- but the folded height is not a size, it is a state.
    t.w = st.drag_from_w;
    if (!tile_shows_folded(t)) t.h = st.drag_from_h;
  }
  st.layout_dirty = true;
  return -1;
}

namespace {
// The last slot taken in the lane this tile belongs to. A tile coming back
// from hidden or from floating goes after it.
int desk_last_slot(const CanvasState& st, const DeskTile& me) {
  int last = -2;
  for (const DeskTile& o : st.tiles) {
    if (&o == &me || o.hidden || o.floating) continue;
    if (tile_is_start_page(o.kind) != tile_is_start_page(me.kind)) continue;
    if (o.col != me.col) continue;
    if (o.slot > last) last = o.slot;
  }
  return last;
}
}  // namespace

void desk_fold(CanvasState& st, int index, bool folded) {
  if (index < 0 || index >= (int)st.tiles.size()) return;
  st.tiles[index].collapsed = folded;
  st.layout_dirty = true;
}

void desk_hide(CanvasState& st, int index) {
  if (index < 0 || index >= (int)st.tiles.size()) return;
  st.tiles[index].hidden = true;
  st.layout_dirty = true;
}

void desk_show(CanvasState& st, TileKind kind) {
  for (DeskTile& t : st.tiles) {
    if (t.kind != kind) continue;
    t.hidden = false;
    // It comes back at the BOTTOM of the lane it belongs to rather than
    // wherever it used to be, so a tile put back cannot land underneath
    // something that has since moved into its place.
    t.slot = desk_last_slot(st, t) + 2;
    st.layout_dirty = true;
    return;
  }
}

void desk_unpin(CanvasState& st, int index, const TileRect& at) {
  if (index < 0 || index >= (int)st.tiles.size()) return;
  DeskTile& t = st.tiles[index];
  // It floats from exactly where the grid had just put it, so unpinning moves
  // nothing on screen — only what happens next.
  t.floating = true;
  t.x = at.x; t.y = at.y; t.w = at.w;
  // Its OPEN height, never the folded one. A folded tile keeps whatever open
  // height it already had (0 means "the default"), so opening it again gives
  // back a tile and not a title bar.
  if (!tile_shows_folded(t)) t.h = at.h;
  st.layout_dirty = true;
}

void desk_pin(CanvasState& st, int index) {
  if (index < 0 || index >= (int)st.tiles.size()) return;
  st.tiles[index].floating = false;
  st.tiles[index].slot = desk_last_slot(st, st.tiles[index]) + 2;
  st.layout_dirty = true;
}

namespace {

// The controls on a tile: fold it away, throw it out, and the corner it is
// resized by. Drawn from rects only — this painter has no glyph for a chevron.
void draw_tile_chrome(HardwareCompositor& S, std::vector<uint32_t>& buf, int iw, int ih,
                      const TileRect& r, bool folded, uint32_t ink) {
  if (r.ctl <= 0) return;
  const int c = r.ctl;
  // Fold: a triangle pointing down when the tile is open, right when it is not.
  const int fx = r.fold_x + 2, fy = r.fold_y + 3;
  if (folded) {
    for (int i = 0; i < 5; ++i) S.draw_rect_to(buf, iw, ih, fx + i, fy + i, 1, 9 - i * 2, ink);
  } else {
    for (int i = 0; i < 5; ++i) S.draw_rect_to(buf, iw, ih, fx + i, fy + i, 9 - i * 2, 1, ink);
  }
  // Throw out: an x.
  const int sx = r.shut_x + 3, sy = r.shut_y + 3;
  for (int i = 0; i < 8; ++i) {
    S.draw_rect_to(buf, iw, ih, sx + i, sy + i, 1, 1, ink);
    S.draw_rect_to(buf, iw, ih, sx + 7 - i, sy + i, 1, 1, ink);
  }
  (void)c;
  // Resize: three steps in the bottom-right corner.
  if (r.grip > 0) {
    // Three steps, drawn from the corner IN, so the pips are inside the tile
    // whatever its grip is — and dark under them, so they are visible over a
    // page as well as over a panel.
    for (int i = 1; i <= 3; ++i) {
      const int g = std::min(r.grip, 14) - i * 3;
      if (g <= 0) continue;
      S.draw_rect_to(buf, iw, ih, r.x + r.w - 5 - g, r.y + r.h - 6, g + 1, 2, 0x66000000);
      S.draw_rect_to(buf, iw, ih, r.x + r.w - 6, r.y + r.h - 5 - g, 2, g + 1, 0x66000000);
      S.draw_rect_to(buf, iw, ih, r.x + r.w - 4 - g, r.y + r.h - 5, g + 1, 1, ink);
      S.draw_rect_to(buf, iw, ih, r.x + r.w - 5, r.y + r.h - 4 - g, 1, g + 1, ink);
    }
  }
}

}  // namespace

// The live transcript, drawn on EITHER face. It was a lambda inside the start
// page's painter, so the desk — the face this machine now opens on — had no
// heartbeat on it at all: a page of state with nothing moving. One painter, two
// callers, the way the power panel works.
// What the mind said to what was asked on THIS face. File scope, like the
// thought stream, because the landing page and the panel grid both want it.
//
// It is not the thought stream: that is the tail of a log the resident appends
// to over its whole life. This is the output of the one command this prompt
// submitted, which is the thing a person who just pressed Enter is waiting for.
// The applications this machine has, and a row per app that launches it. Read
// off the store the same way `app list` does, so the glass and the command
// agree about what is installed.
void draw_apps_tile(HardwareCompositor& S, std::vector<uint32_t>& buf, int iw, int ih,
                    Kernel& k, CanvasState& st, uint64_t now,
                    int box_x, int box_y, int box_w, int box_h) {
  S.draw_rect_to(buf, iw, ih, box_x, box_y, box_w, box_h, 0xF80B0F19);
  S.draw_round_outline_to(buf, iw, ih, box_x, box_y, box_w, box_h, 8, 0xFF334155);
  S.draw_rect_to(buf, iw, ih, box_x + 12, box_y, box_w - 24, 2, 0xFF60A5FA);

  // A store query, not a pixel: once every two seconds is far more often than
  // anybody installs something.
  if (!st.apps_primed || now < st.apps_checked_ms || now - st.apps_checked_ms >= 2000) {
    st.apps_checked_ms = now;
    st.apps_primed = true;
    st.installed_apps.clear();
    // Newest record per name wins, and a name whose newest record is a
    // tombstone stays gone — the same rule `app list` uses. Kept in a vector
    // rather than a map so the order on the glass is the order they were
    // installed rather than the alphabet.
    struct Newest { std::string name, url; uint64_t at = 0; };
    std::vector<Newest> newest;
    for (const auto& id : k.store().find_meta("kind", "app")) {
      auto o = k.store().get(id);
      if (!o.ok()) continue;
      const std::string n = o->meta("app");
      if (n.empty()) continue;
      bool found = false;
      for (auto& e : newest) {
        if (e.name != n) continue;
        found = true;
        if (e.at <= o->timestamp_ns) { e.at = o->timestamp_ns; e.url = o->meta("app") == n ? o->meta("url") : e.url; }
        break;
      }
      if (!found) newest.push_back({n, o->meta("url"), o->timestamp_ns});
    }
    for (const auto& v : newest) {
      if (v.url == "removed") continue;
      CanvasState::AppEntry e;
      e.name = v.name;
      e.url = v.url;
      st.installed_apps.push_back(e);
    }
  }

  st.app_rows.clear();
  const int ctl_room = kTileRight;

  if (box_h < 52) {
    S.draw_label_to(buf, iw, ih, box_x + 16, box_y + 11, "APPS", 0xFF94A3B8, 2);
    const std::string tail = st.installed_apps.empty() ? std::string("none installed")
                                             : std::to_string(st.installed_apps.size()) + " installed";
    const int room = box_w - 32 - S.label_width("APPS", 2) - 24 - ctl_room;
    if (room > 20) {
      const std::string vis = fit(S, tail, room, Face::UiSmall);
      S.draw_text_to(buf, iw, ih, box_x + box_w - 16 - ctl_room - S.text_width(vis, Face::UiSmall),
                     box_y + 10, vis, 0xFFCBD5E1, Face::UiSmall);
    }
    return;
  }

  S.draw_label_to(buf, iw, ih, box_x + 16, box_y + 8, "APPLICATIONS", 0xFF94A3B8, 2);

  int y = box_y + 28;
  const int bottom = box_y + box_h - 6;
  const int room = box_w - 32;

  if (st.installed_apps.empty()) {
    S.draw_text_to(buf, iw, ih, box_x + 16, y,
                   fit(S, "nothing installed", room, Face::UiSmall), 0xFF64748B, Face::UiSmall);
    y += S.line_height(Face::UiSmall) + 2;
    S.draw_text_to(buf, iw, ih, box_x + 16, y,
                   fit(S, "`app install assay` in the shell", room, Face::UiSmall),
                   0xFF475569, Face::UiSmall);
    return;
  }

  const int line = S.line_height(Face::Ui) + 10;
  for (const auto& e : st.installed_apps) {
    if (y + line > bottom) break;
    const bool armed = (st.armed == "app:" + e.name);
    const bool hot = pointer_on(S, iw, box_x + 12, y, box_w - 24, line - 4);
    // A pill per app: the whole row is the target, so a click anywhere on it
    // launches. Armed is not needed — opening an application destroys nothing
    // — so this is one click, unlike a revoke or a rewind. It lights under the
    // pointer, which is the only thing that says it is a control at all.
    S.draw_pill_to(buf, iw, ih, box_x + 12, y, box_w - 24, line - 4,
                   armed ? 0xFF1E293B : (hot ? 0xFF1B2838 : 0xFF111827),
                   hot ? 0xFF50E3C2 : 0xFF334155);
    S.draw_text_to(buf, iw, ih, box_x + 24, y + 4,
                   fit(S, e.name, room - 120, Face::UiBold), hot ? 0xFFFFFFFF : 0xFFE2E8F0, Face::UiBold);
    if (!e.url.empty()) {
      const std::string u = fit(S, e.url, 200, Face::UiSmall);
      const int ux = box_x + box_w - 24 - S.text_width(u, Face::UiSmall);
      if (ux > box_x + 24 + S.text_width(e.name, Face::UiBold) + 12) {
        S.draw_text_to(buf, iw, ih, ux, y + 5, u, 0xFF64748B, Face::UiSmall);
      }
    }
    st.app_rows.push_back({box_x + 12, y, box_w - 24, line - 4, "app:" + e.name, e.name});
    y += line;
  }
}

void draw_answer_tile(HardwareCompositor& S, std::vector<uint32_t>& buf, int iw, int ih,
                      CanvasState& st, uint64_t now,
                      int box_x, int box_y, int box_w, int box_h) {
  S.draw_rect_to(buf, iw, ih, box_x, box_y, box_w, box_h, 0xF80B0F19);
  S.draw_round_outline_to(buf, iw, ih, box_x, box_y, box_w, box_h, 8, 0xFF334155);
  S.draw_rect_to(buf, iw, ih, box_x + 12, box_y, box_w - 24, 2, 0xFF50E3C2);

  const int ctl_room = kTileRight;
  const bool have = !st.answer_lines.empty() || !st.answer_asked.empty();

  // Folded to a strip: say whether anything is happening, and nothing else.
  if (box_h < 52) {
    S.draw_label_to(buf, iw, ih, box_x + 16, box_y + 11, "THE MIND", 0xFF94A3B8, 2);
    std::string tail = st.answer_running ? std::string("working...")
                     : (st.answer_lines.empty() ? std::string("nothing asked yet")
                                                : ascii(st.answer_lines.back()));
    int room = box_w - 32 - S.label_width("THE MIND", 2) - 24 - ctl_room;
    if (room > 20) {
      std::string vis = fit(S, tail, room, Face::UiSmall);
      S.draw_text_to(buf, iw, ih, box_x + box_w - 16 - ctl_room - S.text_width(vis, Face::UiSmall),
                     box_y + 10, vis, 0xFFCBD5E1, Face::UiSmall);
    }
    return;
  }

  S.draw_label_to(buf, iw, ih, box_x + 16, box_y + 8, "WHAT THE MIND SAID", 0xFF94A3B8, 2);

  // A pulse while it is thinking. A person who has pressed Enter needs to know
  // the machine took the question — silence is indistinguishable from ignored,
  // which is exactly what this face used to give them.
  if (st.answer_running) {
    const char* dots[] = {"working", "working.", "working..", "working..."};
    const std::string w = dots[(now / 400) % 4];
    const int wx = box_x + box_w - 16 - ctl_room - S.text_width(w, Face::UiSmall);
    if (wx > box_x + 200) {
      S.draw_text_to(buf, iw, ih, wx, box_y + 8, w, 0xFF50E3C2, Face::UiSmall);
    }
  }

  int y = box_y + 26;
  const int bottom = box_y + box_h - 8;
  const int room = box_w - 32;

  if (!have) {
    S.draw_text_to(buf, iw, ih, box_x + 16, y,
                   fit(S, "Ask above and the answer arrives here.", room, Face::UiSmall),
                   0xFF64748B, Face::UiSmall);
    return;
  }

  // What was asked, so an answer on screen is never orphaned from its question.
  if (!st.answer_asked.empty()) {
    const int lh = S.line_height(Face::UiSmall);
    // Wrapped, up to three lines. A question was CUT at the card's width with
    // an ellipsis — so what a person saw was their own sentence stopping in the
    // middle of a word, above the answer to the part they could not read.
    const std::string q = "asked: " + ascii(st.answer_asked);
    const int space_w = S.text_width(" ", Face::UiSmall);
    std::string line;
    int line_w = 0, drawn = 0;
    auto put_line = [&] {
      if (line.empty() || drawn >= 3 || y + lh > bottom) return;
      S.draw_text_to(buf, iw, ih, box_x + 16, y, line, 0xFF50E3C2, Face::UiSmall);
      y += lh;
      ++drawn;
      line.clear();
      line_w = 0;
    };
    size_t at = 0;
    while (at < q.size() && drawn < 3) {
      const size_t sp = q.find(' ', at);
      const std::string word = q.substr(at, sp == std::string::npos ? std::string::npos : sp - at);
      const int word_w = S.text_width(word, Face::UiSmall);
      const int cand = line.empty() ? word_w : line_w + space_w + word_w;
      if (line.empty() || cand <= room) {
        line = line.empty() ? word : line + " " + word;
        line_w = cand;
      } else {
        put_line();
        line = word;
        line_w = word_w;
      }
      if (sp == std::string::npos) break;
      at = sp + 1;
    }
    // The last line, cut only if the question itself outran three lines.
    if (!line.empty() && drawn < 3 && y + lh <= bottom) {
      const bool more = at < q.size();
      S.draw_text_to(buf, iw, ih, box_x + 16, y,
                     more ? fit(S, line + " …", room, Face::UiSmall) : line,
                     0xFF50E3C2, Face::UiSmall);
      y += lh;
    }
    y += 4;
  }

  // Wrap to the card, then show the rows that fit. Wrapping first is what makes
  // the count of visible rows a count of ROWS: a long answer is one line of
  // output and five rows of reading, and scrolling has to agree with what the
  // eye sees.
  const int lh = S.line_height(Face::Mono);
  std::vector<std::string> rows;
  for (const std::string& raw : st.answer_lines) {
    const std::string line = ascii(raw);
    if (line.empty()) { rows.push_back(std::string()); continue; }
    if (S.text_width(line, Face::Mono) <= room) { rows.push_back(line); continue; }
    size_t p2 = 0;
    std::string cur;
    int cur_w = 0;
    const int space_w = S.text_width(" ", Face::Mono);
    while (p2 < line.size()) {
      size_t sp = line.find(' ', p2);
      std::string word = line.substr(p2, sp == std::string::npos ? std::string::npos : sp - p2);
      while (S.text_width(word, Face::Mono) > room) {
        size_t take = word.size();
        while (take > 1 && S.text_width(word.substr(0, take), Face::Mono) > room) --take;
        if (!cur.empty()) { rows.push_back(cur); cur.clear(); }
        rows.push_back(word.substr(0, take));
        word = word.substr(take);
      }
      // The word measured once and the line's width carried, rather than the
      // whole accumulated line re-measured for every word.
      const int word_w = S.text_width(word, Face::Mono);
      const int cand_w = cur.empty() ? word_w : cur_w + space_w + word_w;
      std::string trial = cur.empty() ? word : cur + " " + word;
      if (cur.empty() || cand_w <= room) { cur = trial; cur_w = cand_w; }
      else { rows.push_back(cur); cur = word; cur_w = word_w; }
      if (sp == std::string::npos) break;
      p2 = sp + 1;
    }
    if (!cur.empty()) rows.push_back(cur);
  }

  const int fits = (bottom - y) / (lh > 0 ? lh : 1);
  int first = 0;
  if ((int)rows.size() > fits) {
    first = (int)rows.size() - fits + st.answer_scroll;
    if (first < 0) first = 0;
    if (first > (int)rows.size() - 1) first = (int)rows.size() - 1;
  }
  for (int i = first; i < (int)rows.size() && y + lh <= bottom; ++i) {
    // Three inks and no more: what the machine said, what it echoed, and what
    // went wrong. Anything cleverer paints confetti down the middle of prose.
    uint32_t ink = 0xFFCBD5E1;
    const std::string& r = rows[i];
    if (r.rfind("answer:", 0) == 0 || r.rfind("Answer:", 0) == 0) ink = 0xFFF1F5F9;
    else if (r.find("error") != std::string::npos || r.find("refused") != std::string::npos ||
             r.find("FAIL") != std::string::npos) ink = 0xFFF87171;
    S.draw_text_to(buf, iw, ih, box_x + 16, y, r, ink, Face::Mono);
    y += lh;
  }
}

void draw_log_tile(HardwareCompositor& S, std::vector<uint32_t>& buf, int iw, int ih,
                   Kernel& k, CanvasState& st, uint64_t now,
                   int box_x, int stream_y, int box_w, int stream_h) {
  const auto fit = [&](const std::string& t, int room, Face face) {
    return ::aios::userland::fit(S, t, room, face);
  };
  {
    S.draw_rect_to(buf, iw, ih, box_x, stream_y, box_w, stream_h, 0xF80B0F19);
    S.draw_round_outline_to(buf, iw, ih, box_x, stream_y, box_w, stream_h, 8, 0xFF334155);
    S.draw_rect_to(buf, iw, ih, box_x + 12, stream_y, box_w - 24, 2, 0xFF34D399);

    // Read back as many lines as the panel can actually show. This was a flat
    // 24 while the panel is now as tall as the display allows — so the newest
    // line sat well short of the bottom edge and the page looked unfinished.
    // One spare line so a partial row at the bottom still has something to draw.
    const int kLogTailKeep = std::max(8, (stream_h - 32) / 15 + 1);
    if (!st.log_primed || now < st.log_checked_ms || now - st.log_checked_ms >= 250) {
      st.log_checked_ms = now;
      st.log_primed = true;
      const std::string log_file = run_dir(k) + "/session.log";
      auto lf = k.host().read_file(log_file);
      st.log_tail.clear();
      if (lf.ok()) {
        const char* data = (const char*)lf->data();
        size_t size = lf->size();
        size_t end = size;
        while (end > 0 && data[end - 1] == '\n') --end;
        std::vector<std::string> rev;
        while (end > 0 && (int)rev.size() < kLogTailKeep) {
          size_t begin = end;
          while (begin > 0 && data[begin - 1] != '\n') --begin;
          if (end > begin) rev.emplace_back(data + begin, end - begin);
          end = begin ? begin - 1 : 0;
          while (end > 0 && data[end - 1] == '\n') --end;
          if (begin == 0) break;
        }
        st.log_tail.assign(rev.rbegin(), rev.rend());
      }
    }
    const std::vector<std::string>& lines = st.log_tail;
    const int ctl_room = kTileRight;

    if (stream_h < 52) {
      S.draw_label_to(buf, iw, ih, box_x + 16, stream_y + 11, "THOUGHT STREAM", 0xFF94A3B8, 2);
      std::string tail = lines.empty() ? std::string("nothing on the log yet") : ascii(lines.back());
      int room = box_w - 32 - S.label_width("THOUGHT STREAM", 2) - 24 - ctl_room;
      if (room > 20) {
        std::string vis = fit(tail, room, Face::UiSmall);
        S.draw_text_to(buf, iw, ih, box_x + box_w - 16 - ctl_room - S.text_width(vis, Face::UiSmall),
                       stream_y + 10, vis, 0xFFCBD5E1, Face::UiSmall);
      }
      return;
    }
    // Measured against the room it actually has. The long form ran straight
    // under the tile's own fold and close controls in a one-column tile.
    {
      const char* kLong = "LIVE THOUGHT STREAM & KERNEL SUBSTRATE LOG";
      const char* kShort = "THOUGHT STREAM";
      const int room = box_w - 32 - kTileRight;
      const char* label = S.label_width(kLong, 2) <= room ? kLong : kShort;
      S.draw_label_to(buf, iw, ih, box_x + 16, stream_y + 8, label, 0xFF94A3B8, 2);
    }

    int ly = stream_y + 26;
    int line_h = 15;
    int max_lines = (stream_h - 32) / line_h;
    int start_idx = lines.size() > (size_t)max_lines ? (int)lines.size() - max_lines : 0;
    int log_room = box_w - 32;
    if (lines.empty()) {
      S.draw_text_to(buf, iw, ih, box_x + 16, ly, fit("Resident mind entity ready on kernel substrate", log_room, Face::UiSmall), 0xFFCBD5E1, Face::UiSmall);
    } else {
      for (size_t i = start_idx; i < lines.size() && ly + line_h <= stream_y + stream_h - 6; ++i) {
        std::string l = lines[i];
        // This used to REPLACE the line where the mind said what it could not
        // do with "Substrate ready: repository mounted at /repos/aios, C++
        // execution fabric online" — an invented boast written over the
        // machine's own admission of failure. It says what it said.
        if (!readable_log_line(l)) continue;
        std::string vis_l = fit(l, log_room, Face::Mono);
        uint32_t col = 0xFFCBD5E1;
        if (l.find("mind working on") != std::string::npos || l.find("Answer:") != std::string::npos) col = 0xFF38BDF8;
        else if (l.find("thinking") != std::string::npos) col = 0xFF50E3C2;
        else if (l.find("Substrate ready") != std::string::npos || l.find("PASS") != std::string::npos) col = 0xFF34D399;
        else if (l.find("mind") != std::string::npos) col = 0xFF50E3C2;
        else if (l.find("ERROR") != std::string::npos || l.find("Refused") != std::string::npos) col = 0xFFF87171;
        S.draw_text_to(buf, iw, ih, box_x + 16, ly, vis_l, col, Face::Mono);
        ly += line_h;
      }
    }
  }
}

// Defined below, beside the rest of the inspector: both faces draw it, and the
// start page's painter comes first in this file.
static void draw_inspect_tile(HardwareCompositor& S, std::vector<uint32_t>& buf, int iw, int ih,
                              Kernel& k, CanvasState& st, uint64_t now, int x, int y, int w, int h);

// --- The emblem's orbit, drawn properly ------------------------------------
//
// What was here: three satellites and six "celestial energy motes", each a
// 4x4 or 6x6 call to draw_pill_to, on six unrelated ellipses at three
// unrelated tilts. At that size a rounded rect has no round left in it, and
// ButtonSuite::blend_pixel forces the destination alpha to 0xFF — so over the
// shader background (where the overlay is cleared transparent and composited
// on the device) every one of them landed as an opaque hard-edged SQUARE.
// Scattered, aliased, clumped to one side, popping in and out with no path
// under them and no depth between them. It read as debris, not as an orbit.
//
// What makes a ring of dots read as an orbit is the ring: the PATH, the fact
// that a body passing behind the thing it orbits goes BEHIND it and dims, and
// a trail that says which way it is going. So: two tilted rings, drawn in
// halves either side of the emblem; bodies on those rings, sized and lit by
// depth, occluded by the core on the far side; a short tapering trail each.

// One pixel of glow, correct on BOTH compositing paths. On a machine whose
// background is a shader, the overlay buffer starts transparent and the device
// composites it over the scene, so this has to be a real source-over that
// keeps an alpha; on a machine painting its own background it is the additive
// blend the halo above uses. Getting this wrong is what made the old dots
// opaque squares.
static void emblem_glow_px(std::vector<uint32_t>& buf, int iw, int ih, int x, int y,
                           uint32_t rgb, float a, bool on_gpu) {
  if (a <= 0.003f || x < 0 || x >= iw || y < 0 || y >= ih) return;
  if (a > 1.0f) a = 1.0f;
  const uint32_t sr = (rgb >> 16) & 0xFF, sg = (rgb >> 8) & 0xFF, sb = rgb & 0xFF;
  uint32_t& px = buf[(size_t)y * iw + x];
  const uint32_t d = px;
  if (on_gpu) {
    const float da = ((d >> 24) & 0xFF) / 255.0f;
    const float oa = a + da * (1.0f - a);
    if (oa <= 0.0001f) return;
    const float w = da * (1.0f - a);
    const uint32_t r = (uint32_t)((sr * a + ((d >> 16) & 0xFF) * w) / oa);
    const uint32_t g = (uint32_t)((sg * a + ((d >> 8) & 0xFF) * w) / oa);
    const uint32_t b = (uint32_t)((sb * a + (d & 0xFF) * w) / oa);
    px = ((uint32_t)(oa * 255.0f) << 24) | (std::min(255u, r) << 16) |
         (std::min(255u, g) << 8) | std::min(255u, b);
  } else {
    const uint32_t r = std::min(255u, (uint32_t)(((d >> 16) & 0xFF) + sr * a));
    const uint32_t g = std::min(255u, (uint32_t)(((d >> 8) & 0xFF) + sg * a));
    const uint32_t b = std::min(255u, (uint32_t)((d & 0xFF) + sb * a));
    px = 0xFF000000u | (r << 16) | (g << 8) | b;
  }
}

// A body on the ring: a soft disc with a falloff, not a square. `r` is the
// core radius; it carries its own glow out to about twice that.
static void emblem_body(std::vector<uint32_t>& buf, int iw, int ih,
                        float cx, float cy, float r, uint32_t rgb, float bright,
                        bool on_gpu) {
  if (r <= 0.1f || bright <= 0.01f) return;
  const float reach = r * 2.6f;
  const int x0 = (int)std::floor(cx - reach), x1 = (int)std::ceil(cx + reach);
  const int y0 = (int)std::floor(cy - reach), y1 = (int)std::ceil(cy + reach);
  for (int y = y0; y <= y1; ++y) {
    const float dy = (float)y + 0.5f - cy;
    for (int x = x0; x <= x1; ++x) {
      const float dx = (float)x + 0.5f - cx;
      const float d = std::sqrt(dx * dx + dy * dy);
      float a;
      if (d <= r) {
        a = 1.0f;                                   // solid core
      } else if (d <= r + 1.0f) {
        a = 1.0f - (d - r);                         // one pixel of coverage
      } else {
        const float g = 1.0f - (d - r) / (reach - r);
        if (g <= 0.0f) continue;
        a = 0.42f * g * g * g;                      // the halo it casts
      }
      emblem_glow_px(buf, iw, ih, x, y, rgb, a * bright, on_gpu);
    }
  }
}

// Half of a tilted orbit ring. `from`/`to` are angles along the ring, so the
// half that passes behind the emblem can be laid down before the emblem is
// drawn and the half in front after it. Stepped by arc length so the stroke
// stays even all the way round instead of bunching at the ends of the major
// axis.
static void emblem_ring_arc(std::vector<uint32_t>& buf, int iw, int ih,
                            float cx, float cy, float rx, float ry, float tilt,
                            float from, float to, uint32_t rgb, float bright,
                            bool on_gpu) {
  const float ct = std::cos(tilt), stl = std::sin(tilt);
  const int steps = std::max(24, (int)((rx + ry) * 1.6f));
  for (int i = 0; i <= steps; ++i) {
    const float ang = from + (to - from) * ((float)i / (float)steps);
    const float ex = rx * std::cos(ang), ey = ry * std::sin(ang);
    const float px = cx + ex * ct - ey * stl;
    const float py = cy + ex * stl + ey * ct;
    // Brighter where the ring comes toward the viewer, so the tilt reads.
    const float lean = 0.55f + 0.45f * (0.5f + 0.5f * std::sin(ang));
    const int bx = (int)std::floor(px), by = (int)std::floor(py);
    const float fx = px - bx, fy = py - by;      // bilinear, so the arc is smooth
    const float a = bright * lean;
    emblem_glow_px(buf, iw, ih, bx,     by,     rgb, a * (1 - fx) * (1 - fy), on_gpu);
    emblem_glow_px(buf, iw, ih, bx + 1, by,     rgb, a * fx * (1 - fy), on_gpu);
    emblem_glow_px(buf, iw, ih, bx,     by + 1, rgb, a * (1 - fx) * fy, on_gpu);
    emblem_glow_px(buf, iw, ih, bx + 1, by + 1, rgb, a * fx * fy, on_gpu);
  }
}

static void draw_unblink_mind_start_page(HardwareCompositor& S, std::vector<uint32_t>& buf, int iw, int ih,
                                          Kernel& k, uint64_t now, CanvasState& st,
                                          const CanvasLayout& lay, const CanvasFeed& f) {
  // ---------------------------------------------------------------------------
  // The top bar is the COMPOSITOR's, drawn over this face and the windowed
  // ones alike — see draw_top_bar. This page used to paint a second one of its
  // own: brand pill, clock, MIND/CAPS and a power button, none of which the
  // other bar had and all of which it now carries. Two bars reporting one
  // machine is how they came to disagree.
  //
  // What stays is the measurement, because everything below is laid out from
  // it: topbar_h is the room the shared bar occupies, not a bar drawn here.
  const CanvasChrome chrome = canvas_chrome(S, iw);
  const int topbar_h = chrome.topbar_h;

  // 1. Hero Header & Animated Celestial Orbital Emblem & Aura
  // ---------------------------------------------------------------------------
  int box_w = std::min(1040, iw - 48);
  int hero_y = topbar_h + 16;
  int emblem_cx = iw / 2;
  int emblem_cy = hero_y + 28;

  const float t_sec = (now % 86400000ULL) * 0.001f;
  const float pulse = st.answer_running
                        ? 0.90f + 0.10f * std::sin(t_sec * 4.0f)
                        : 0.92f + 0.08f * std::sin(t_sec * 1.5f);

  // Soft spherical radiant halo around the emblem and hero title
  const float halo_rad_x = 180.0f * pulse;
  const float halo_rad_y = 130.0f * pulse;
  const int min_gx = std::max(0, (int)(emblem_cx - halo_rad_x));
  const int max_gx = std::min(iw - 1, (int)(emblem_cx + halo_rad_x));
  const int min_gy = std::max(topbar_h + 1, (int)(emblem_cy - halo_rad_y));
  const int max_gy = std::min(ih - 1, (int)(emblem_cy + halo_rad_y + 35));

  // The halo's whole per-pixel job -- falloff, shimmer, colour mix -- is a
  // function of the squared distance from the centre and nothing else, so it
  // is tabulated once per frame over d2 and each pixel becomes a multiply and
  // an index. It was a std::sqrt AND a std::sin per pixel over ~94,000 pixels,
  // every frame, on the CPU, to draw a glow that is radially symmetric.
  constexpr int kHaloLut = 192;
  float lut_f[kHaloLut + 1];
  uint32_t lut_r[kHaloLut + 1], lut_g[kHaloLut + 1], lut_b[kHaloLut + 1];
  for (int i = 0; i <= kHaloLut; ++i) {
    const float d2 = (float)i / (float)kHaloLut;
    const float r = std::sqrt(d2);
    const float smooth_falloff = (1.0f - d2) * (1.0f - d2);
    const float shimmer = 1.0f + 0.08f * std::sin(r * 12.0f - t_sec * 3.0f);
    lut_f[i] = 0.45f * smooth_falloff * shimmer;
    const float inner_mix = std::clamp(1.0f - r * 1.6f, 0.0f, 1.0f);
    lut_r[i] = (uint32_t)(40 * (1.0f - inner_mix) + 52 * inner_mix);
    lut_g[i] = (uint32_t)(160 * (1.0f - inner_mix) + 211 * inner_mix);
    lut_b[i] = (uint32_t)(248 * (1.0f - inner_mix) + 180 * inner_mix);
  }

  for (int gy = min_gy; gy <= max_gy; ++gy) {
    const float dy = (gy - emblem_cy) / halo_rad_y;
    const float dy2 = dy * dy;
    if (dy2 >= 1.0f) continue;
    const float edge_fade = std::min(1.0f, (float)(gy - topbar_h) / 36.0f);
    // Only the columns whose d2 can be under 1 at all, instead of testing the
    // full bounding box a row at a time.
    const float half = halo_rad_x * std::sqrt(1.0f - dy2);
    const int x_lo = std::max(min_gx, (int)(emblem_cx - half));
    const int x_hi = std::min(max_gx, (int)(emblem_cx + half));
    uint32_t* row = &buf[(size_t)gy * iw];
    for (int gx = x_lo; gx <= x_hi; ++gx) {
      const float dx = (gx - emblem_cx) / halo_rad_x;
      const float d2 = dx * dx + dy2;
      if (d2 >= 1.0f) continue;
      const int li = (int)(d2 * kHaloLut);
      const float factor = lut_f[li] * edge_fade;
      if (factor <= 0.0f) continue;
      const uint32_t glow_r = lut_r[li], glow_g = lut_g[li], glow_b = lut_b[li];
      if (st.background_on_gpu) {
        const uint32_t a = (uint32_t)(factor * 255.0f);
        row[gx] = (a << 24) | (glow_r << 16) | (glow_g << 8) | glow_b;
      } else {
        const uint32_t bg = row[gx];
        const uint32_t nr = std::min(255u, ((bg >> 16) & 0xFF) + (uint32_t)(glow_r * factor));
        const uint32_t ng = std::min(255u, ((bg >> 8) & 0xFF) + (uint32_t)(glow_g * factor));
        const uint32_t nb = std::min(255u, (bg & 0xFF) + (uint32_t)(glow_b * factor));
        row[gx] = 0xFF000000 | (nr << 16) | (ng << 8) | nb;
      }
    }
  }

  // Animated Multi-Ring Orbital Emblem with Rotating Nodes
  //
  // Two rings, one plane, shared centre. Everything on the FAR side of the
  // emblem is laid down first, the emblem is drawn over it, and the near side
  // goes on top — which is the whole difference between a thing being orbited
  // and a thing with dots sprinkled round it.
  const bool on_gpu = st.background_on_gpu;
  const float tilt = -0.32f;                       // one plane, not three

  // The emblem carries the machine's state and NOT its numbers. An orbit
  // cannot encode a percentage -- there is no baseline on the glass to read it
  // against -- but it can be seen from across a room, which is the one thing a
  // number on a bar cannot do. So: colour, and under strain it slows and dims
  // rather than speeding up. A machine in trouble should spend LESS on its own
  // decoration, and a desktop whose animation gets busy exactly when the
  // machine is busy is a feedback loop, not an instrument.
  const LoadState load_state = st.machine_load.state();
  const uint32_t ring_ink = load_state == LoadState::Alarmed  ? 0xFFEF4444
                          : load_state == LoadState::Strained ? 0xFFF59E0B
                                                              : 0x00000000;   // 0 = leave each ring its own colour
  const float load_rate = load_state == LoadState::Alarmed  ? 0.35f
                        : load_state == LoadState::Strained ? 0.65f : 1.0f;
  const float load_dim  = load_state == LoadState::Alarmed  ? 0.72f
                        : load_state == LoadState::Strained ? 0.86f : 1.0f;
  auto tint = [&](uint32_t own) { return ring_ink ? ring_ink : own; };
  struct Orbit { float rx, ry, speed, phase, r; uint32_t rgb; };
  // Slow. The old ones ran at 1.2-2.0 rad/s -- a third of a revolution every
  // second, which is not an orbit, it is a fidget.
  // Both rings clear the emblem's 36px rim. The inner one was 46 and the
  // emblem is 32 across the core with a rim on top of that, so five sixths of
  // it was swallowed: a body on it spent its orbit hidden and surfaced as a
  // stub either side.
  const Orbit orbits[] = {
    { 60.0f, 21.0f,  0.46f, 0.0f,        2.6f, 0xFF7DD3FC },
    { 88.0f, 31.0f, -0.31f, 2.0943951f,  2.2f, 0xFF34D399 },
    { 88.0f, 31.0f, -0.31f, 4.1887902f,  1.7f, 0xFF50E3C2 },
  };
  const int kOrbits = (int)(sizeof(orbits) / sizeof(orbits[0]));

  // Where a body is, and how near the viewer. sin(ang) runs +1 at the front of
  // the ring to -1 at the back, which is the depth cue everything below uses.
  auto orbit_at = [&](const Orbit& o, float ang, float* px, float* py, float* depth) {
    const float ct = std::cos(tilt), stl = std::sin(tilt);
    const float ex = o.rx * std::cos(ang), ey = o.ry * std::sin(ang);
    *px = emblem_cx + ex * ct - ey * stl;
    *py = emblem_cy + ex * stl + ey * ct;
    *depth = std::sin(ang);                        // +1 near, -1 far
  };

  // A body with its trail. Near the viewer it is bigger, brighter and warmer;
  // far side it shrinks and dims, so the ring reads as going round rather than
  // sliding side to side.
  auto draw_body = [&](const Orbit& o, float ang, bool near_half) {
    float px, py, depth;
    orbit_at(o, ang, &px, &py, &depth);
    if ((depth >= 0.0f) != near_half) return;
    const float lit = 0.45f + 0.55f * (0.5f + 0.5f * depth);
    // The trail, behind it along the ring, tapering away.
    const float dir = (o.speed >= 0.0f) ? -1.0f : 1.0f;
    for (int t = 1; t <= 9; ++t) {
      const float tf = (float)t / 9.0f;
      float tx, ty, td;
      orbit_at(o, ang + dir * tf * 0.42f, &tx, &ty, &td);
      if ((td >= 0.0f) != near_half) continue;
      const float fade = (1.0f - tf) * (1.0f - tf);
      emblem_body(buf, iw, ih, tx, ty, o.r * (0.55f + 0.30f * (1.0f - tf)) * (0.72f + 0.28f * (0.5f + 0.5f * td)),
                  tint(o.rgb), fade * 0.34f * lit * load_dim, on_gpu);
    }
    emblem_body(buf, iw, ih, px, py, o.r * (0.78f + 0.34f * (0.5f + 0.5f * depth)),
                tint(o.rgb), lit * load_dim, on_gpu);
  };

  // --- far side: the half of each ring that passes BEHIND the emblem --------
  for (int i = 0; i < kOrbits; ++i) {
    const Orbit& o = orbits[i];
    if (i == 2) continue;                          // two rings, three bodies
    emblem_ring_arc(buf, iw, ih, emblem_cx, emblem_cy, o.rx, o.ry, tilt,
                    3.14159265f, 6.28318531f, tint(o.rgb), 0.22f * load_dim, on_gpu);
  }
  for (int i = 0; i < kOrbits; ++i)
    draw_body(orbits[i], t_sec * orbits[i].speed * load_rate + orbits[i].phase, false);

  // --- the emblem itself ----------------------------------------------------
  float core_pulse = 1.0f + 0.05f * std::sin(t_sec * 2.5f);
  // Circles, drawn as circles. These were four draw_pill_to calls -- a rounded
  // RECTANGLE asked for a corner radius equal to half its side, which is a
  // circle only if the corner is exact. It is not: the outline stipples, and
  // the emblem wore a dashed green ring nobody drew on purpose.
  const float core_r = 32.0f * core_pulse;
  ButtonSuite::draw_circle(buf, iw, ih, (float)emblem_cx, (float)emblem_cy, core_r, 0xFF0F172A, tint(0xFF36B37E), 1.5f);
  ButtonSuite::draw_circle(buf, iw, ih, (float)emblem_cx, (float)emblem_cy, 24.0f, 0xFF1E293B, 0xFF50E3C2, 1.0f);
  ButtonSuite::draw_circle(buf, iw, ih, (float)emblem_cx, (float)emblem_cy, 16.0f, 0xFF0F172A, 0xFF38BDF8, 1.0f);
  ButtonSuite::draw_circle(buf, iw, ih, (float)emblem_cx, (float)emblem_cy,  8.0f, 0xFF34D399, 0xFF38BDF8, 1.0f);

  // --- near side: the half of each ring that passes IN FRONT ----------------
  for (int i = 0; i < kOrbits; ++i) {
    const Orbit& o = orbits[i];
    if (i == 2) continue;
    emblem_ring_arc(buf, iw, ih, emblem_cx, emblem_cy, o.rx, o.ry, tilt,
                    0.0f, 3.14159265f, tint(o.rgb), 0.38f * load_dim, on_gpu);
  }
  for (int i = 0; i < kOrbits; ++i)
    draw_body(orbits[i], t_sec * orbits[i].speed * load_rate + orbits[i].phase, true);

  // The six "floating celestial energy motes" that used to be scattered here
  // on six more unrelated ellipses are gone. They were the same 4px squares,
  // they shared no plane with the orbits or with each other, and six of them
  // around one emblem is not a system, it is confetti.

  // Main Hero Typography
  std::string title_str = fit(S, "AIOS", std::max(10, iw - 32), Face::UiBold);
  int title_w = S.text_width(title_str, Face::UiBold);
  S.draw_text_to(buf, iw, ih, std::max(16, emblem_cx - title_w / 2), emblem_cy + 40, title_str, 0xFFFFFFFF, Face::UiBold);

  // No tagline under the name: a start page that describes itself in
  // marketing copy is one that has nothing to say about the machine.

  // ---------------------------------------------------------------------------
  // 2. Centered Mind Intelligence Exchange Glass Bar
  // ---------------------------------------------------------------------------
  int box_x = (iw - box_w) / 2;
  int box_y = emblem_cy + 82;
  int box_h = 56;

  S.draw_rect_to(buf, iw, ih, box_x, box_y, box_w, box_h, 0xF80B132B);
  S.draw_round_outline_to(buf, iw, ih, box_x, box_y, box_w, box_h, 12, 0xFF36B37E);
  S.draw_round_outline_to(buf, iw, ih, box_x - 1, box_y - 1, box_w + 2, box_h + 2, 13,
                          st.prompt_focused ? 0xFF7DD3FC : 0xFF38BDF8);

  // Action Buttons on Right (Ask Mind), the pill sized from its own label.
  int btn_w = chrome.ask_w, btn_h = chrome.ask_h;
  int btn1_x = box_x + box_w - btn_w - 10;
  int btn_y = box_y + (box_h - btn_h) / 2;
  S.draw_pill_to(buf, iw, ih, btn1_x, btn_y, btn_w, btn_h, 0xFF059669, 0xFF34D399);
  const std::string btn1_str = kAskLabel;
  S.draw_text_to(buf, iw, ih, btn1_x + (btn_w - S.text_width(btn1_str, Face::UiBold)) / 2,
                 btn_y + (btn_h - S.line_height(Face::UiBold)) / 2, btn1_str, 0xFFFFFFFF, Face::UiBold);

  // Status tag pill to button's left. The engine's name is read here now: it
  // used to come from the locals the page's own top bar had already computed,
  // and that bar is gone.
  const std::string mind_name = bound_mind_name(k);
  const std::string model_top = mind_name.empty() ? std::string("OFFLINE") : upper(ascii(mind_name));
  const bool mind_up = !mind_name.empty();
  std::string tag_str = mind_up ? ("ENGINE: " + model_top + " - ONLINE")
                                : std::string("ENGINE: NONE BOUND - OFFLINE");
  int tag_w = S.text_width(tag_str, Face::UiSmall);
  int tag_h = 26;
  int tag_pill_w = 18 + tag_w + 12;
  int tag_x = btn1_x - tag_pill_w - 12;
  int tag_y = box_y + (box_h - tag_h) / 2;
  bool draw_tag = (tag_x > box_x + 220);
  if (draw_tag) {
    // Green means a mind answered this kernel. An unbound slot is grey, so the
    // glass cannot show a lit status light for something that is not there.
    S.draw_pill_to(buf, iw, ih, tag_x, tag_y, tag_pill_w, tag_h,
                   mind_up ? 0xFF064E3B : 0xFF1E293B, mind_up ? 0xFF059669 : 0xFF475569);
    S.draw_pill_to(buf, iw, ih, tag_x + 8, tag_y + (tag_h - 8) / 2, 8, 8,
                   mind_up ? 0xFF10B981 : 0xFF64748B, mind_up ? 0xFF34D399 : 0xFF94A3B8);
    S.draw_text_to(buf, iw, ih, tag_x + 22, tag_y + (tag_h - S.line_height(Face::UiSmall)) / 2,
                   tag_str, mind_up ? 0xFF34D399 : 0xFF94A3B8, Face::UiSmall);
  }

  // Prompt symbol & input line
  int prompt_y = box_y + (box_h - S.line_height(Face::Ui)) / 2;
  S.draw_text_to(buf, iw, ih, box_x + 18, prompt_y, "aios::mind>", 0xFF50E3C2, Face::UiBold);
  // The line the person is actually typing. Empty and unfocused shows the
  // invitation; empty and focused shows nothing but the caret.
  const bool prompt_live = st.prompt_focused;
  std::string prompt_input = st.prompt.empty()
      ? (prompt_live ? std::string("") : std::string("Ask or command AIOS resident mind..."))
      : ascii(st.prompt.text);
  int prompt_x_offset = box_x + 18 + S.text_width("aios::mind>", Face::UiBold) + 10;
  int right_bound = draw_tag ? tag_x - 12 : btn1_x - 12;
  int prompt_room = right_bound - prompt_x_offset;
  if (prompt_room > 20) {
    // A person typing past the right edge must keep seeing what they are
    // typing, so an overlong line scrolls. It scrolls to keep the CURSOR in
    // view rather than the end of the line: with the cursor moved back to fix
    // the front of a long line, holding the tail on screen would hide the very
    // character being typed.
    size_t drop = 0;
    std::string vis_prompt = prompt_input;
    while (!vis_prompt.empty() && S.text_width(vis_prompt, Face::Ui) > prompt_room) {
      vis_prompt.erase(vis_prompt.begin());
      ++drop;
    }
    size_t cursor = st.prompt.empty() ? 0 : st.prompt.cursor;
    if (cursor < drop) {
      drop = cursor;
      vis_prompt.assign(prompt_input, drop, std::string::npos);
      while (!vis_prompt.empty() && S.text_width(vis_prompt, Face::Ui) > prompt_room) vis_prompt.pop_back();
    }
    S.draw_text_to(buf, iw, ih, prompt_x_offset, prompt_y, vis_prompt,
                   st.prompt.empty() && !prompt_live ? 0xFF64748B : 0xFFF8FAFC, Face::Ui);
    if (prompt_live && (now / 450) % 2 == 0) {
      size_t shown = cursor > drop ? cursor - drop : 0;
      if (shown > vis_prompt.size()) shown = vis_prompt.size();
      int caret_x = prompt_x_offset + S.text_width(vis_prompt.substr(0, shown), Face::Ui) + 2;
      if (caret_x + 2 <= right_bound - 4)
        S.draw_rect_to(buf, iw, ih, caret_x, prompt_y + 1, 2, S.line_height(Face::Ui) - 2, 0xFF50E3C2);
    }
  }

  // ---------------------------------------------------------------------------
  // 2.5 Quick Theme & Background Scene Picker Chips
  // ---------------------------------------------------------------------------
  struct ThemeChipDef {
    const char* label;
    int scene;
    const char* desc;
    uint32_t active_color;
  };
  const ThemeChipDef theme_chips[] = {
    {"Ocean Dusk", 4, "ocean at dusk sunset open horizon water waves orange purple twilight glow", 0xFFF59E0B},
    {"Beach Moon", 0, "", 0xFF38BDF8},
    {"Cyber Grid", 1, "", 0xFF10B981},
    {"Quantum Orb", 2, "", 0xFFA855F7},
    {"Pyramids", 3, "three pyramids in the desert dunes sand starlight night sky", 0xFFEC4899},
    {"Full View", -1, "", 0xFF38BDF8},
  };

  int chip_bar_y = box_y + box_h + 10;
  int chip_bar_h = 24;
  int cur_scene = st.scene(DeskView::StartPage);

  int total_chips_w = 0;
  int chip_widths[6];
  for (int i = 0; i < 6; ++i) {
    chip_widths[i] = S.text_width(theme_chips[i].label, Face::UiSmall) + 20;
    total_chips_w += chip_widths[i] + (i > 0 ? 8 : 0);
  }

  int cur_chip_x = box_x + (box_w - total_chips_w) / 2;

  for (int i = 0; i < 6; ++i) {
    bool is_active = (theme_chips[i].scene == cur_scene);
    if (theme_chips[i].scene == -1) is_active = st.bare_scene;

    uint32_t bg_col = is_active ? 0xF01E293B : 0xD00F172A;
    uint32_t border_col = is_active ? theme_chips[i].active_color : 0xFF334155;
    uint32_t text_col = is_active ? 0xFFFFFFFF : 0xFF94A3B8;

    S.draw_pill_to(buf, iw, ih, cur_chip_x, chip_bar_y, chip_widths[i], chip_bar_h, bg_col, border_col);
    if (is_active) {
      S.draw_pill_to(buf, iw, ih, cur_chip_x + 6, chip_bar_y + 8, 6, 6, theme_chips[i].active_color, 0xFFFFFFFF);
    }
    int txt_x = cur_chip_x + (is_active ? 16 : (chip_widths[i] - S.text_width(theme_chips[i].label, Face::UiSmall)) / 2);
    int txt_y = chip_bar_y + (chip_bar_h - S.line_height(Face::UiSmall)) / 2;
    S.draw_text_to(buf, iw, ih, txt_x, txt_y, theme_chips[i].label, text_col, Face::UiSmall);

    cur_chip_x += chip_widths[i] + 8;
  }

  // ---------------------------------------------------------------------------
  // 2.6 Which provider and model the mind is on
  // ---------------------------------------------------------------------------
  // Two selects, the same pair the GUI's picker offers. The open list is drawn
  // at the END of this page, over the tiles, because a list that appears under
  // the thing below it is not a list.
  {
    const MindSelect m = mind_select(S, k, st, box_x, box_w, chip_bar_y + chip_bar_h + 12);
    const auto select = [&](const char* field, int x, int w, const std::string& value, bool open) {
      const int lw = S.label_width(field, 1);
      S.draw_label_to(buf, iw, ih, x - lw - 8, m.y + (m.h - 10) / 2, field, 0xFF64748B, 1);
      const bool hot = open || pointer_on(S, iw, x, m.y, w, m.h);
      S.draw_pill_to(buf, iw, ih, x, m.y, w, m.h,
                     open ? 0xF01E293B : 0xD00F172A, hot ? 0xFF60A5FA : 0xFF334155);
      S.draw_text_to(buf, iw, ih, x + 14, m.y + (m.h - S.line_height(Face::UiSmall)) / 2,
                     fit(S, value, w - 40, Face::UiSmall),
                     hot ? 0xFFFFFFFF : 0xFFCBD5E1, Face::UiSmall);
      // The arrow: a select says it opens.
      const int ax = x + w - 20, ay = m.y + m.h / 2 - 1;
      for (int i = 0; i < 4; ++i)
        S.draw_rect_to(buf, iw, ih, ax + i, ay + i, 8 - 2 * i, 1, hot ? 0xFFFFFFFF : 0xFF94A3B8);
    };
    select("PROVIDER", m.prov_x, m.prov_w, m.provider, m.which == 1);
    select("MODEL", m.model_x, m.model_w, m.model, m.which == 2);
  }

  // ---------------------------------------------------------------------------
  // 3. The desk: every card and panel this face still carries, where the
  //    person who uses this machine has left it. What used to be here was a
  //    fixed 3x2 array of six cards with two full-width panels nailed under
  //    them; nothing on it could be moved, folded or thrown out.
  // ---------------------------------------------------------------------------
  struct HeroFace {
    TileKind kind;
    const char* key;
    const char* title;
    const char* desc;
    uint32_t accent;
    const char* mode_badge;
  };
  const HeroFace hero_faces[] = {
    {TileKind::HeroEditor,  "[F4]", "Code Editor",    "C++ Workspace & Native IDE",      0xFF34D399, "HUMAN"},
    {TileKind::HeroDb,      "[F5]", "DB Studio",      "MariaDB & SQL Explorer",          0xFFA855F7, "HUMAN"},
    {TileKind::HeroWeb,     "[F6]", "Unblink Web",    "This Kernel's Own Web Engine",     0xFF38BDF8, "HUMAN"},
    {TileKind::HeroTensor,  "[P]",  "Tensor HAL",     "OpenCL Compute Graph",            0xFFF59E0B, "MIND"},
    {TileKind::HeroHorizon, "[2]",  "Horizon Vector", "Hacker News & Security Stream",   0xFFEC4899, "MIND"},
    {TileKind::HeroBuild,   "[3]",  "Auto Build",     "Build -> Test -> Git Commit Loop",0xFF10B981, "CO-PILOT"},
  };

  auto hero_card = [&](TileKind kind, int cx, int cy, int card_w, int card_h) {
    const HeroFace* hf = &hero_faces[0];
    for (const HeroFace& face : hero_faces) if (face.kind == kind) { hf = &face; break; }

    // Deep slate glass card background & outline
    S.draw_rect_to(buf, iw, ih, cx, cy, card_w, card_h, 0xF80F172A);
    S.draw_round_outline_to(buf, iw, ih, cx, cy, card_w, card_h, 8, 0xFF334155);
    // Vibrant 3px top accent bar
    S.draw_rect_to(buf, iw, ih, cx + 8, cy, card_w - 16, 3, hf->accent);

    // Folded: the key and the name on one line, and nothing else. A tile a
    // person has put away still has to say which one it is.
    if (card_h < 52) {
      int kw = S.text_width(hf->key, Face::UiBold);
      int ky = cy + (card_h - S.line_height(Face::UiBold)) / 2;
      S.draw_text_to(buf, iw, ih, cx + 12, ky, hf->key, hf->accent, Face::UiBold);
      int room = card_w - 12 - kw - 10 - kTileRight;
      if (room > 10)
        S.draw_text_to(buf, iw, ih, cx + 12 + kw + 10, ky, fit(S, hf->title, room, Face::UiBold),
                       0xFFF8FAFC, Face::UiBold);
      return;
    }

    // Key badge icon box on left with glowing accent border & fill
    int badge = std::min(44, card_h - 28);
    S.draw_rect_to(buf, iw, ih, cx + 12, cy + 14, badge, badge, 0xFF020617);
    S.draw_round_outline_to(buf, iw, ih, cx + 12, cy + 14, badge, badge, 6, hf->accent);
    int kw = S.text_width(hf->key, Face::UiBold);
    S.draw_text_to(buf, iw, ih, cx + 12 + (badge - kw) / 2,
                   cy + 14 + (badge - S.line_height(Face::UiBold)) / 2, hf->key, hf->accent, Face::UiBold);

    // No status dot. These cards are keyboard shortcuts, not services, and a
    // green light on all six was painted unconditionally -- it reported nothing
    // and was true of nothing.

    // Card title & subtitle. The mode badge sits under them on the right now:
    // the top-right corner belongs to the fold and remove controls.
    int mb_w = S.text_width(hf->mode_badge, Face::UiSmall);
    int title_room = card_w - 64 - kTileRight;
    int desc_room = card_w - 64 - mb_w - 28;
    if (title_room > 10) {
      S.draw_text_to(buf, iw, ih, cx + 64, cy + 16, fit(S, hf->title, title_room, Face::UiBold), 0xFFF8FAFC, Face::UiBold);
    }
    if (desc_room > 10) {
      S.draw_text_to(buf, iw, ih, cx + 64, cy + 38, fit(S, hf->desc, desc_room, Face::UiSmall), 0xFFCBD5E1, Face::UiSmall);
    }
    S.draw_text_to(buf, iw, ih, cx + card_w - 16 - mb_w, cy + 38, hf->mode_badge, 0xFF94A3B8, Face::UiSmall);
  };

  // ---------------------------------------------------------------------------
  // Workstation & Co-Pilot Telemetry HUD
  // ---------------------------------------------------------------------------
  auto telemetry_tile = [&](int tile_x, int ws_y, int tile_w, int ws_h) {
    const WorkstationState& a = st.apps;
    S.draw_rect_to(buf, iw, ih, tile_x, ws_y, tile_w, ws_h, 0xF80F172A);
    S.draw_round_outline_to(buf, iw, ih, tile_x, ws_y, tile_w, ws_h, 8, 0xFF334155);
    S.draw_rect_to(buf, iw, ih, tile_x + 12, ws_y, tile_w - 24, 2, 0xFF38BDF8);

    // Row one: Editor workspace state
    std::string ed;
    uint32_t ed_col = 0xFFCBD5E1;
    if (!a.has_editor) {
      ed = "Workspace C++ Editor ready (Press F4 to open active source pane)";
    } else if (a.editor_path.empty()) {
      ed = "Scratch buffer active, " + dec(a.editor_lines) + " line(s) - /repos/aios";
    } else {
      ed = ascii(a.editor_path) + "  " + dec(a.editor_lines) + " ln  " +
           dec(a.editor_bytes) + " B  Ln " + dec((uint64_t)a.editor_cursor_line + 1) +
           (a.editor_dirty ? "  MODIFIED" : "  SAVED");
      ed_col = a.editor_dirty ? kWarn : 0xFF50E3C2;
    }

    // Row two: SQL Database Studio state
    std::string db;
    uint32_t db_col = 0xFFCBD5E1;
    if (!a.has_db) {
      db = "MariaDB Native Wire Client ready (Press F5 for Navicat DB Explorer)";
    } else if (!a.db_connected) {
      db = "MariaDB local socket ready - /run/mysqld/mysqld.sock [DISCONNECTED]";
      db_col = kWarn;
    } else {
      db = dec(a.db_tables) + " table(s)" +
           (a.db_selected.empty() ? std::string() : "  " + ascii(a.db_selected)) + "  " +
           dec(a.db_rows) + "x" + dec(a.db_columns) + "  " + ascii(a.db_status);
      db_col = a.db_error ? kErr : 0xFF50E3C2;
    }

    const int ctl_room = kTileRight;
    if (ws_h < 52) {
      // Folded: the label, and the one reading that changed most recently.
      S.draw_label_to(buf, iw, ih, tile_x + 16, ws_y + 11, "WORKSTATION TELEMETRY", 0xFF94A3B8, 2);
      std::string one = a.has_editor ? ed : db;
      int room = tile_w - 32 - S.label_width("WORKSTATION TELEMETRY", 2) - 24 - ctl_room;
      if (room > 20) {
        std::string vis = fit(S, one, room, Face::UiSmall);
        S.draw_text_to(buf, iw, ih, tile_x + tile_w - 16 - ctl_room - S.text_width(vis, Face::UiSmall),
                       ws_y + 10, vis, a.has_editor ? ed_col : db_col, Face::UiSmall);
      }
      return;
    }
    S.draw_label_to(buf, iw, ih, tile_x + 16, ws_y + 9, "WORKSTATION TELEMETRY & SUBSTRATE LOG", 0xFF94A3B8, 2);
    // The value column starts after the WIDEST label, measured. It was a flat
    // 66px, which fits "EDIT" and "SQL" and is painted straight through by
    // "SUBSTRATE".
    int label_col = 0;
    for (const char* l : {"EDIT", "SQL", "HOST", "KERNEL", "SUBSTRATE"})
      label_col = std::max(label_col, S.label_width(l, 1));
    const int value_x = tile_x + 16 + label_col + 14;
    int hud_room = tile_x + tile_w - ctl_room - 16 - value_x;
    S.draw_label_to(buf, iw, ih, tile_x + 16, ws_y + 28, "EDIT", 0xFF94A3B8, 1);
    if (hud_room > 20) {
      S.draw_text_to(buf, iw, ih, value_x, ws_y + 26, fit(S, ed, hud_room, Face::Mono), ed_col, Face::Mono);
    }
    if (ws_h >= 68) {
      S.draw_label_to(buf, iw, ih, tile_x + 16, ws_y + 49, "SQL", 0xFF94A3B8, 1);
      if (hud_room > 20) {
        S.draw_text_to(buf, iw, ih, value_x, ws_y + 47, fit(S, db, hud_room, Face::Mono), db_col, Face::Mono);
      }
    }
    // AND THE SUBSTRATE, which this tile has promised in its own title since it
    // was written and has never once shown: two rows about the workstation's
    // panes, and then however much empty card the layout gave it. Everything
    // here is read from the kernel and the host at the moment it is drawn —
    // nothing is a plausible-looking constant, which is the failure mode a
    // status surface falls into when nobody checks it.
    int sy = ws_y + 70;
    const int line = S.line_height(Face::Mono) + 6;
    auto row = [&](const char* label, const std::string& value, uint32_t ink) {
      if (sy + line > ws_y + ws_h - 6) return;
      S.draw_label_to(buf, iw, ih, tile_x + 16, sy + 2, label, 0xFF94A3B8, 1);
      if (hud_room > 20)
        S.draw_text_to(buf, iw, ih, value_x, sy, fit(S, value, hud_room, Face::Mono), ink, Face::Mono);
      sy += line;
    };
    if (ws_h >= 96) {
      const auto& topo = k.host().topology();
      const uint64_t total_mib = topo.total_memory >> 20;
      const uint64_t used_mib = (topo.total_memory - topo.available_memory) >> 20;
      row("HOST", topo.arch + "  " + dec(topo.logical_cpus) + " cpu  " + dec(used_mib) + " / " +
                     dec(total_mib) + " MiB  up " + ms_short(k.uptime_ns() / 1000000ull),
          0xFFCBD5E1);
      row("KERNEL", dec(k.entities().list().size()) + " entities  " + dec(k.states().list().size()) +
                        " states  " + dec(k.store().count()) + " objects  " +
                        dec(k.capabilities().list({}).size()) + " capabilities",
          0xFF50E3C2);
      // What only a substrate that counts these knows. Bare metal fills them;
      // a hosted kernel does not, and a zero it did not measure is not printed.
      std::string sub;
      if (f.ticks) sub += dec(f.ticks) + " ticks at " + dec(f.tick_hz) + " Hz  ";
      if (f.switches) sub += dec(f.switches) + " switches  ";
      if (f.pages_total) sub += dec(f.pages_live) + " / " + dec(f.pages_total) + " pages  ";
      if (f.heap_kib) sub += dec(f.heap_kib) + " KiB heap  ";
      if (f.rx_frames || f.tx_frames) sub += "net " + dec(f.rx_frames) + " rx / " + dec(f.tx_frames) + " tx";
      if (!sub.empty()) row("SUBSTRATE", sub, 0xFFCBD5E1);
      else row("SUBSTRATE", "this kernel runs on a host - ticks, pages and frames are the host's to count",
               0xFF64748B);
    }
  };

  // ---------------------------------------------------------------------------
  // Live Co-Pilot Thought Stream
  // ---------------------------------------------------------------------------
  const auto stream_tile = [&](int x, int y, int w, int h) {
    draw_log_tile(S, buf, iw, ih, k, st, now, x, y, w, h);
  };

  for (const TileRect& r : lay.tiles) {
    if (r.index < 0 || r.index >= (int)st.tiles.size()) continue;
    const bool folded = r.h <= kFoldH;
    switch (r.kind) {
      case TileKind::Telemetry:     telemetry_tile(r.x, r.y, r.w, r.h); break;
      case TileKind::ThoughtStream: stream_tile(r.x, r.y, r.w, r.h); break;
      case TileKind::MindAnswer:    draw_answer_tile(S, buf, iw, ih, st, now, r.x, r.y, r.w, r.h); break;
      case TileKind::Apps:          draw_apps_tile(S, buf, iw, ih, k, st, now, r.x, r.y, r.w, r.h); break;
      case TileKind::AppPane:       S.paint_app_tile(buf, iw, ih, k, r, lay.scale); break;
      case TileKind::Inspect:       draw_inspect_tile(S, buf, iw, ih, k, st, now, r.x, r.y, r.w, r.h); break;
      default:                      hero_card(r.kind, r.x, r.y, r.w, r.h); break;
    }
    draw_tile_chrome(S, buf, iw, ih, r, folded, 0xFF64748B);
  }

  // The open select's list, LAST: it hangs over whatever is under it, and a
  // list drawn before the tiles would be behind them.
  if (st.mind_select_open) {
    const MindSelect m = mind_select(S, k, st, box_x, box_w, chip_bar_y + chip_bar_h + 12);
    if (!m.list.empty()) {
      const int lh = (int)m.list.size() * m.row_h + 8;
      S.draw_rect_to(buf, iw, ih, m.list_x, m.list_y, m.list_w, lh, 0xF80B0F19);
      S.draw_round_outline_to(buf, iw, ih, m.list_x, m.list_y, m.list_w, lh, 8, 0xFF475569);
      const std::string& cur = (m.which == 1) ? m.provider : m.model;
      for (size_t i = 0; i < m.list.size(); ++i) {
        const int ry = m.list_y + 4 + (int)i * m.row_h;
        const bool on = m.list[i] == cur;
        const bool hot = pointer_on(S, iw, m.list_x + 2, ry, m.list_w - 4, m.row_h);
        if (on || hot)
          S.draw_rect_to(buf, iw, ih, m.list_x + 2, ry, m.list_w - 4, m.row_h,
                         on ? 0xF0134E4A : 0xFF1E293B);
        S.draw_text_to(buf, iw, ih, m.list_x + 14, ry + (m.row_h - S.line_height(Face::UiSmall)) / 2,
                       fit(S, m.list[i], m.list_w - 28, Face::UiSmall),
                       on ? 0xFF99F6E4 : (hot ? 0xFFFFFFFF : 0xFFCBD5E1), Face::UiSmall);
      }
    }
  }

  // The bottom of the glass belongs to the taskbar now — the machine's one
  // strip, drawn over every face by the compositor. There used to be a status
  // pill here reading "SPACE: the desk . F1: the workspace", which named a
  // face this machine no longer has and sat underneath the taskbar besides.
}

// ---------------------------------------------------------------------------
// The inspector: one identity, and what this kernel knows about it
// ---------------------------------------------------------------------------
// Every panel on the desk prints 128-bit ids and then does nothing with them.
// That is the whole difference between a screen that reports and a screen that
// IS the machine: the id is the handle. This walks the registries the ids came
// from — entities, capabilities, states, channels, the object store — and says
// what the kernel holds about the one that was clicked, including the two
// things no other OS keeps: who granted this authority, and what produced this
// object.
//
// Built on a clock and cached, not per frame: it is registry walks and store
// reads, not pixels.
static void build_inspect(Kernel& k, CanvasState& st) {
  st.inspect_rows.clear();
  st.inspect_title.clear();
  st.inspect_built = st.inspect_id;
  if (st.inspect_id.empty()) return;
  const std::string want = st.inspect_id;
  auto row = [&](const std::string& label, const std::string& value) {
    st.inspect_rows.push_back({label, value, false, "", ""});
  };
  // A row that names another identity: clicking it moves the inspector there.
  auto link = [&](const std::string& label, const std::string& value,
                  const std::string& id, const std::string& kind) {
    st.inspect_rows.push_back({label, value, false, id, kind});
  };
  auto head = [&](const std::string& label) {
    st.inspect_rows.push_back({label, "", true, "", ""});
  };
  auto short_of = [](const std::string& full) {
    return full.size() > 12 ? full.substr(0, 12) : full;
  };

  if (st.inspect_kind == "entity") {
    for (const auto& e : k.entities().list()) {
      if (e.id.str() != want) continue;
      st.inspect_title = e.label.empty() ? std::string("entity") : e.label;
      row("STATUS", entity_status_name(e.status));
      row("GENERATION", dec(e.generation));
      row("STATES", dec(e.states.size()));
      row("MEMORY", dec(e.memory.size()) + " object(s)");
      row("HELD", dec(e.capabilities.size()) + " capabilities");
      if (!e.execution.is_null()) row("EXECUTING", e.execution.short_str() + "  at " + dec(e.execution_position));
      if (e.checkpoints || e.restores)
        row("HISTORY", dec(e.checkpoints) + " checkpoint(s), " + dec(e.restores) + " restore(s)");
      // What it may do. The rights, in the kernel's own words.
      const auto caps = k.capabilities().list(e.id);
      if (!caps.empty()) {
        head("AUTHORITY");
        for (const auto& c : caps) {
          std::string v = rights_str(c.rights);
          if (c.scope_limit) v += "   " + dec(c.scope_used / 1024) + " / " + dec(c.scope_limit / 1024) + " KiB";
          if (!c.parent.is_null()) v += "   from " + c.parent.short_str();
          link(c.resource.str(), v, c.id.str(), "capability");
        }
      }
      if (!e.relationships.empty()) {
        head("RELATED");
        for (const auto& r : e.relationships)
          link(relation_kind_name(r.kind), r.peer.short_str(), r.peer.str(), "entity");
      }
      // And what it has PUT INTO THE WORLD: every object in the store whose
      // provenance is this entity. This is the answer to "what has it done",
      // which on every other machine is a log file at best.
      size_t made = 0;
      std::vector<std::pair<std::string, std::string>> latest;   // id, what it is
      for (const auto& oid : k.store().list()) {
        auto got = k.store().get(oid);
        if (!got.ok() || got->provenance.str() != want) continue;
        ++made;
        if (latest.size() < 8) {
          std::string title = got->meta("title");
          if (title.empty()) title = got->meta("kind");
          if (title.empty()) title = dec(got->payload.size()) + " bytes";
          latest.emplace_back(oid.str(), oid.short_str() + "  " + title);
        }
      }
      head("PRODUCED");
      row("OBJECTS", dec(made));
      for (const auto& l : latest) link("", l.second, l.first, "knows");
      return;
    }
    st.inspect_title = "no entity with this identity";
    return;
  }

  if (st.inspect_kind == "capability") {
    for (const auto& c : k.capabilities().list({})) {
      if (c.id.str() != want) continue;
      st.inspect_title = c.resource.str() + "  " + rights_str(c.rights);
      link("HOLDER", c.holder.short_str(), c.holder.str(), "entity");
      row("RESOURCE", c.resource.str());
      row("RIGHTS", rights_str(c.rights));
      if (c.scope_limit) row("SCOPE", dec(c.scope_used) + " / " + dec(c.scope_limit));
      row("EXPIRES", c.expires_ns ? dec(c.expires_ns) : std::string("never"));
      // Where it CAME FROM. Nothing mints authority: every capability but the
      // kernel's own root is a child of another one, and this is that chain.
      head("DERIVED FROM");
      std::string parent = c.parent.str();
      int depth = 0;
      while (!parent.empty() && parent.find_first_not_of("0") != std::string::npos && depth++ < 8) {
        bool found = false;
        for (const auto& p : k.capabilities().list({})) {
          if (p.id.str() != parent) continue;
          link(short_of(parent), p.resource.str() + "  " + rights_str(p.rights) + "   held by " + p.holder.short_str(),
               parent, "capability");
          parent = p.parent.str();
          found = true;
          break;
        }
        if (!found) { row(short_of(parent), "revoked, or the kernel's own root"); break; }
      }
      if (depth == 0) row("", "minted by the kernel itself");
      return;
    }
    st.inspect_title = "revoked - this capability is no longer held";
    return;
  }

  if (st.inspect_kind == "state") {
    for (const auto& s : k.states().list()) {
      if (s.id.str() != want) continue;
      st.inspect_title = std::string("state  ") + state_kind_name(s.kind);
      link("OWNER", s.owner.short_str(), s.owner.str(), "entity");
      row("KIND", state_kind_name(s.kind));
      row("GENERATION", dec(s.generation));
      row("PAYLOAD", dec(s.payload.size()) + " bytes inline");
      if (!s.memory.empty()) row("MEMORY", dec(s.memory.size()) + " object(s)");
      return;
    }
    st.inspect_title = "no state with this identity";
    return;
  }

  if (st.inspect_kind == "knows") {
    for (const auto& oid : k.store().list()) {
      if (oid.str() != want) continue;
      auto got = k.store().get(oid);
      if (!got.ok()) { st.inspect_title = "this object cannot be read back"; return; }
      const StoredObject& o = *got;
      std::string title = o.meta("title");
      st.inspect_title = title.empty() ? std::string("object") : title;
      link("PRODUCED BY", o.provenance.short_str(), o.provenance.str(), "entity");
      row("PAYLOAD", dec(o.payload.size()) + " bytes");
      if (!o.embedding.empty()) row("EMBEDDING", dec(o.embedding.size()) + " dimensions");
      // The history chain: what this was made from, walked back.
      if (!o.derived_from.is_null()) {
        head("DERIVED FROM");
        ObjectID at = o.derived_from;
        for (int depth = 0; depth < 8 && !at.is_null(); ++depth) {
          auto up = k.store().get(at);
          if (!up.ok()) { row(at.short_str(), "no longer in the store"); break; }
          std::string t = up->meta("title");
          link(at.short_str(), t.empty() ? dec(up->payload.size()) + " bytes" : t, at.str(), "knows");
          at = up->derived_from;
        }
      }
      if (!o.metadata.empty()) {
        head("METADATA");
        for (const auto& m : o.metadata) row(m.first, m.second);
      }
      if (!o.relations.empty()) {
        head("RELATED");
        for (const auto& r : o.relations)
          link(dec(r.kind), r.target.short_str(), r.target.str(), "knows");
      }
      return;
    }
    st.inspect_title = "no object with this identity";
    return;
  }

  if (st.inspect_kind == "channel") {
    for (const auto& c : k.channels().list()) {
      if (c.id.str() != want) continue;
      st.inspect_title = c.transport.empty() ? std::string("channel") : c.transport;
      link("OWNER", c.owner.short_str(), c.owner.str(), "entity");
      row("REGION", c.region.short_str());
      row("CAPACITY", dec(c.capacity) + " slots of " + dec(c.slot_bytes) + " bytes");
      row("PUBLISHED", dec(c.published) + " frame(s)");
      row("CONSUMERS", dec(c.cursors.size()));
      return;
    }
    st.inspect_title = "no channel with this identity";
    return;
  }
  st.inspect_title = "nothing on this machine holds that identity";
}

// The tile itself. Label on the left in the console's field voice, value on the
// right in the machine's; a heading is a rule with a word on it.
static void draw_inspect_tile(HardwareCompositor& S, std::vector<uint32_t>& buf, int iw, int ih,
                              Kernel& k, CanvasState& st, uint64_t now, int x, int y, int w, int h) {
  if (st.inspect_built != st.inspect_id || !st.inspect_checked_ms ||
      now < st.inspect_checked_ms || now - st.inspect_checked_ms >= 1000) {
    st.inspect_checked_ms = now;
    build_inspect(k, st);
  }
  // Its own ground. A tile with no card under it is a hole: the panels behind
  // it read straight through the text, and on the display path there is nothing
  // there at all but the scene.
  S.draw_rect_to(buf, iw, ih, x, y, w, h, 0xF80B0F19);
  S.draw_round_outline_to(buf, iw, ih, x, y, w, h, 8, 0xFF334155);
  S.draw_rect_to(buf, iw, ih, x + 12, y, w - 24, 2, kMint);

  const int pad = 14;
  int yy = y + 14;
  const std::string kind = upper(st.inspect_kind.empty() ? std::string("nothing") : st.inspect_kind);
  S.draw_label_to(buf, iw, ih, x + pad, yy, kind, kMint, 2);
  if (!st.inspect_title.empty()) {
    const int tw = S.label_width(kind, 2) + 14;
    S.draw_text_to(buf, iw, ih, x + pad + tw, yy - 2,
                   fit(S, st.inspect_title, w - pad * 2 - tw, Face::UiBold), kText, Face::UiBold);
  }
  yy += 20;
  // The identity itself, whole. Twelve characters is what fits on a panel row;
  // this is the page where the other twenty belong.
  S.draw_text_to(buf, iw, ih, x + pad, yy, fit(S, st.inspect_id, w - pad * 2, Face::Mono), kFaint, Face::Mono);
  yy += S.line_height(Face::Mono) + 8;
  S.draw_rect_to(buf, iw, ih, x + pad, yy, w - pad * 2, 1, kBorder);
  yy += 8;

  const int line = S.line_height(Face::UiSmall) + 6;
  const int label_w = std::min(180, (w - pad * 2) / 3);
  for (const auto& r : st.inspect_rows) {
    if (yy > y + h - line) break;
    if (r.head) {
      yy += 4;
      S.draw_label_to(buf, iw, ih, x + pad, yy + 2, r.label, kMint, 2);
      const int lw = S.label_width(r.label, 2);
      if (w - pad * 2 - lw - 10 > 0)
        S.draw_rect_to(buf, iw, ih, x + pad + lw + 10, yy + 7, w - pad * 2 - lw - 10, 1, kBorder);
      yy += line;
      continue;
    }
    const bool link = !r.id.empty();
    const bool hot = link && pointer_on(S, iw, x + pad - 4, yy - 2, w - pad * 2 + 8, line);
    if (hot) {
      S.draw_rect_to(buf, iw, ih, x + pad - 4, yy - 2, w - pad * 2 + 8, line, kPanel);
      S.draw_rect_to(buf, iw, ih, x + pad - 4, yy - 2, 2, line, kMint);
    }
    if (link) st.identity_rows.push_back({x + pad - 4, yy - 2, w - pad * 2 + 8, line, r.id, r.kind});
    if (!r.label.empty())
      S.draw_label_to(buf, iw, ih, x + pad, yy + 2, fit(S, r.label, label_w, Face::UiSmall),
                      hot ? kMint : kFaint, 1);
    S.draw_text_to(buf, iw, ih, x + pad + label_w + 12, yy,
                   fit(S, r.value, w - pad * 2 - label_w - 12, Face::Mono),
                   hot ? kText : (link ? 0xFFCBD5E1 : kDim), Face::Mono);
    yy += line;
  }
}

void paint_canvas(HardwareCompositor& S, std::vector<uint32_t>& out, uint32_t W, uint32_t H, Kernel& k, Surface& console,
                  const CanvasFeed& f, uint64_t now, CanvasState& st) {
  if (!W || !H) return;
  if (out.size() != (size_t)W * H) out.assign((size_t)W * H, kBg);
  // The desk, before anything is drawn on it. Reading and writing the saved
  // arrangement happens HERE because this is the one place on the canvas's
  // path that is handed a Kernel; the mouse handlers only ever set the dirty
  // flag. A host that cannot write the file keeps its arrangement for this
  // run and loses it on the next one, rather than failing the frame.
  if (!st.tiles_seeded) seed_desk_tiles(st);
  if (!st.layout_loaded) {
    st.layout_loaded = true;
    auto lf = k.host().read_file(desk_layout_path(k));
    if (lf.ok()) desk_layout_parse(st, std::string((const char*)lf->data(), lf->size()));
  }
  desk_layout_save(st, k);
  // The frame rate is measured by the presenting loop, not here. Measuring it
  // off consecutive PAINTS was right only while the overlay was repainted every
  // frame; now that a still interface skips its repaint, the gap between paints
  // is the caret blink and the bar read "2 FPS" on a desktop presenting sixty.
  // See run_desktop_gui, which stamps st.fps from the loop it actually paces.
  const uint32_t s = (W >= 3000 || H >= 1700) ? 2 : 1;
  const uint32_t w = W / s, h = H / s;
  if (s != 1 && st.scale_scratch.size() != (size_t)w * h) st.scale_scratch.assign((size_t)w * h, kBg);
  std::vector<uint32_t>& buf = (s == 1) ? out : st.scale_scratch;
  const int iw = (int)w, ih = (int)h;
  // ONE source for where every tile is. The painter below draws from this and
  // HardwareCompositor's mouse handlers hit-test the very same rects.
  const CanvasLayout lay = canvas_layout(S, W, H, st);

  // Which view this frame is painting, and therefore which background it is
  // entitled to. The desk and the start page are two views of this machine, not
  // one with a flag: each keeps its own.
  const DeskView view = lay.start_page ? DeskView::StartPage : DeskView::Desk;
  // The start page carries exactly one text field, and it used to take the
  // keyboard only when clicked -- so typing at a freshly opened start page went
  // to a workspace window that is not even on the screen. Arriving on this view
  // gives the prompt the keyboard; Escape hands it back to the shortcut keys.
  if (lay.start_page != st.start_page_shown) {
    st.start_page_shown = lay.start_page;
    st.prompt_focused = lay.start_page;
  }
  const char* env_s = getenv("AOS_DESKTOP_SCENE");
  int current_scene = st.scene(view);
  // A scene named on the command line is the OPERATOR asking, at startup — so
  // it is honoured once, written into the desk state like any other explicit
  // choice, and then it stops talking. It used to be re-applied on every frame,
  // which clamped the screen: a machine started with `--beach` read a later
  // `display show cyber`, applied it, and had it overwritten a frame later, so
  // the person asking got nothing and no saved choice could ever survive. Who
  // owns the choice after startup is whoever asks next.
  if (env_s && *env_s && !st.env_scene_applied) {
    st.env_scene_applied = true;
    int s_env = -1;
    if (!strcmp(env_s, "cyber") || !strcmp(env_s, "neural") || !strcmp(env_s, "1")) s_env = 1;
    else if (!strcmp(env_s, "orb") || !strcmp(env_s, "quantum") || !strcmp(env_s, "2")) s_env = 2;
    else if (!strcmp(env_s, "beach") || !strcmp(env_s, "0")) s_env = 0;
    if (s_env >= 0) { st.set_every_scene(s_env); st.layout_dirty = true; }
  }
  {
    // Look at the run directory at most twice a second. Reading it on every
    // frame put a filesystem round trip inside the paint; what a person can
    // tell the difference between is the scene changing within half a second,
    // which this still does.
    // The FIRST look after a restart only adopts what the run directory says as
    // the baseline; it does not re-apply it. Who owns the choice on a restart
    // is the persisted desk state (`scene start/desk/workspace` in the saved
    // layout), which is where every explicit selection — command, switcher
    // chip, context menu — was written. Re-applying a stale run-directory file
    // over it brought the machine back on a scene nobody last asked for.
    bool adopt_only = false;
    if (st.scene_file_value < 0 || now < st.scene_checked_ms || now - st.scene_checked_ms >= 500) {
      st.scene_checked_ms = now;
      // Once per run, on the first look, whether or not there IS a file: after
      // this the operator's next explicit request applies normally.
      if (!st.scene_file_baselined) { st.scene_file_baselined = true; adopt_only = true; }
      // Which FACE to show, beside which scene to draw. Asked to show a
      // pyramid, this machine used to draw one behind twelve panels that
      // covered it — being told to show something and then covering it is not
      // showing it. `display show <name>` writes "scene" here and the desk
      // comes back on `display desk`, on the space bar, or on a click.
      {
        const std::string face_file = run_dir(k) + "/face";
        auto ff = k.host().read_file(face_file);
        if (ff.ok() && ff->size() > 0) {
          std::string fv((const char*)ff->data(), ff->size());
          const size_t b2 = fv.find_first_not_of(" \t\r\n");
          const size_t e2 = (b2 == std::string::npos) ? b2 : fv.find_first_of(" \t\r\n", b2);
          fv = (b2 == std::string::npos) ? std::string()
                                         : fv.substr(b2, (e2 == std::string::npos) ? std::string::npos : e2 - b2);
          // Only when it CHANGES: the file names a face to show, it does not
          // own which face this machine is on for the rest of time.
          if (fv != st.face_file_applied) {
            st.face_file_applied = fv;
            if (fv == "scene") { st.bare_scene = true; st.hide_panels = true; st.collapsed_panels = false; }
            else if (fv == "start") { st.bare_scene = false; st.hide_panels = true; st.collapsed_panels = false; }
            else if (fv == "desk" || fv == "panels") { st.bare_scene = false; st.hide_panels = false; st.collapsed_panels = false; }
          }
        }
      }
      const std::string scene_file = run_dir(k) + "/scene";
      auto sf = k.host().read_file(scene_file);
      st.scene_file_value = -1;
      if (sf.ok()) {
        // The whole request, which is a name and (from `display show`) a serial
        // on the second line: a repeat of the same scene is a NEW request and
        // must be honoured, or a machine already on "beach" cannot be told to
        // show the beach. The name is the first word; the serial only makes the
        // request different.
        const std::string whole((const char*)sf->data(), sf->size());
        if (whole != st.scene_file_request) {
          st.scene_file_request = whole;
          st.scene_file_applied = -2;   // a new request: apply whatever it names
        }
        std::string s_val = whole;
        size_t b = s_val.find_first_not_of(" \t\r\n");
        size_t e = (b == std::string::npos) ? b : s_val.find_first_of(" \t\r\n", b);
        s_val = (b == std::string::npos) ? std::string() : s_val.substr(b, (e == std::string::npos) ? std::string::npos : e - b);
        // The rest of the FIRST line, if there is any, is the hour the scene
        // was asked for: `display show beach at dusk` writes "beach 780". It is
        // read whatever scene is named, so switching away and back does not
        // quietly put the shoreline into a light nobody asked for.
        // Not on the first look either: the hour comes back with the desk.
        if (!adopt_only && b != std::string::npos && e != std::string::npos) {
          size_t eol1 = whole.find('\n', e);
          std::string rest = whole.substr(e, (eol1 == std::string::npos) ? std::string::npos : eol1 - e);
          size_t rb = rest.find_first_not_of(" \t\r");
          if (rb != std::string::npos) {
            size_t re = rest.find_first_of(" \t\r", rb);
            std::string word = rest.substr(rb, (re == std::string::npos) ? std::string::npos : re - rb);
            bool digits = !word.empty();
            for (char ch : word) if (ch < '0' || ch > '9') { digits = false; break; }
            if (digits) {
              int v = 0;
              for (char ch : word) { v = v * 10 + (ch - '0'); if (v > 1000) { v = 1000; break; } }
              st.scene_tod = (float)v / 1000.0f;
              st.layout_dirty = true;
            } else {
              // A file somebody wrote by hand says the word, not the number.
              bool named = false;
              const float tv = time_of_day_of(word, &named);
              if (named) { st.scene_tod = tv; st.layout_dirty = true; }
            }
          }
        }
        if (s_val == "cyber" || s_val == "neural" || s_val == "matrix" || s_val == "lattice" || s_val == "1") st.scene_file_value = 1;
        else if (s_val == "orb" || s_val == "quantum" || s_val == "core" || s_val == "plasma" || s_val == "2") st.scene_file_value = 2;
        else if (s_val == "pyramid" || s_val == "desert" || s_val == "3") st.scene_file_value = 3;
        else if (s_val == "beach" || s_val == "ocean" || s_val == "tide" || s_val == "moon" || s_val == "0") st.scene_file_value = 0;
        // A scene this machine made up rather than one it was compiled with.
        // The name in the scene file is only a pointer: the description sits
        // beside it, because it is far too much to put in one word. Off means
        // off: a request for one is left where the machine already was,
        // rather than drawn once more and then never asked for.
        else if ((s_val == "composed" || s_val == "4") && st.ai_scenes_enabled) {
          std::string desc_file = scene_file + ".composed";
          auto df = k.host().read_file(desc_file);
          if (df.ok() && df->size() > 0) {
            std::string text((const char*)df->data(), df->size());
            if (text != st.composed_text) {
              st.composed_text = text;
              ++st.composed_generation;
            }
            st.scene_file_value = 4;
          }
          // No description means nothing to draw, so the scene it was on
          // stays: a half-written file must not blank the display.
        }
      }
    }
    // The run directory NAMES a scene to show; it does not own what each view
    // is on. So it lands on the view that is up, once, when it changes — which
    // is what `display show <name>` means — and a background picked here or
    // from the switcher afterwards is not overwritten half a second later.
    if (adopt_only && st.scene_file_value >= 0) {
      // Booting onto the saved scene, not onto a re-read of the last request.
      st.scene_file_applied = st.scene_file_value;
    } else if (st.scene_file_value >= 0 && st.scene_file_value != st.scene_file_applied) {
      st.scene_file_applied = st.scene_file_value;
      // EVERY view, not the one that happens to be up on the frame the file is
      // read. This applied to `view` alone and then latched, and the view on
      // that frame is not the view you are looking at: the layout is not loaded
      // yet on the first frame, so it reports the start page, the request landed
      // there, and every frame after it painted the desk — which had never been
      // given the scene. Asked for a picture, the machine put it on a face
      // nobody was in front of and went on showing the beach. A request written
      // to the run directory names a scene for THIS MACHINE and carries no view
      // with it, which is what set_every_scene is for.
      st.set_every_scene(st.scene_file_value);
      // Saved with the desk, or the machine goes back to the scene it was on
      // before the request the moment it is restarted.
      st.layout_dirty = true;
    }
    current_scene = st.scene(view);
  }

  // What the display path needs to know, whoever is drawing the background.
  st.scene_effective = current_scene;

  // The beach used to be kept OFF the device: the shader's scene 0 was `tide`,
  // a generic ocean by the same name, so on a machine with a GPU asking for
  // the beach put a different picture on the glass. The answer to that was to
  // hand scene 0 back to the painter — and the painter's beach is a STILL,
  // cached once per display size and copied from then on, because recomputing
  // a 1920x1080 sea per frame costs a third of a core. A still sea is the one
  // thing a sea cannot be, and a beach with no cloud moving over it is a
  // photograph of a beach.
  //
  // The shader now draws THIS shoreline — the same world-space sea and
  // specular moon path, plus a cloud deck and a wash that runs up the sand —
  // and it draws them moving, at 1/2 resolution, for a few watts. So the
  // device gets scene 0 back. A machine with no GPU still gets the painter's
  // still, which is the honest trade there: it has nothing to march it with.
  if (st.background_on_gpu) {
    // The background is a function of coordinates and the clock, which is what
    // a GPU is for and what this painter spends a third of its frame on. When
    // the display path is computing it in a shader, leave those pixels CLEAR:
    // the interface is drawn over them as usual and the shader fills whatever
    // the interface did not cover. Anything drawn here with alpha therefore
    // composites over the scene on the device instead of over a colour guessed
    // here, which is what makes a panel read as glass over moving water.
    std::fill(buf.begin(), buf.end(), 0x00000000u);
  } else {
    paint_scene(buf, iw, ih, now, st, current_scene);
  }

  auto finish = [&]() {
    if (s == 2) {
      // Doubling is the only scale this painter uses, so it needs no division
      // per pixel and only half the row work: widen one row, then hand the
      // duplicate row a memcpy of it.
      for (uint32_t sy = 0; sy < h; ++sy) {
        const uint32_t* src = &st.scale_scratch[(size_t)sy * w];
        uint32_t* dst = &out[(size_t)(sy * 2) * W];
        for (uint32_t sx = 0; sx < w; ++sx) {
          const uint32_t px = src[sx];
          dst[sx * 2] = px;
          dst[sx * 2 + 1] = px;
        }
        if (w * 2 < W) std::fill(dst + w * 2, dst + W, src[w - 1]);
        const uint32_t y2 = sy * 2 + 1;
        if (y2 < H) std::memcpy(&out[(size_t)y2 * W], dst, (size_t)W * sizeof(uint32_t));
      }
      for (uint32_t y = h * 2; y < H; ++y)
        std::memcpy(&out[(size_t)y * W], &out[(size_t)(h * 2 - 1) * W], (size_t)W * sizeof(uint32_t));
    } else if (s != 1) {
      for (uint32_t y = 0; y < H; ++y) {
        const uint32_t* src = &st.scale_scratch[(size_t)(y / s) * w];
        uint32_t* dst = &out[(size_t)y * W];
        for (uint32_t x = 0; x < W; ++x) dst[x] = src[x / s];
      }
    }
    st.frame++;
  };

  // AOS_SHOW_PANELS forces the panel grid, which is how `canvas-dump` gets a
  // look at the panels on a surface whose state defaults to the start page.
  // Which face is up is decided in ONE place, so the layout and the paint
  // cannot pick different ones.
  if (st.bare_scene) {
    finish();
    return;
  }

  if (lay.start_page) {
    draw_unblink_mind_start_page(S, buf, iw, ih, k, now, st, lay, f);
    finish();
    return;
  }

  if (iw < 420 || ih < 300) {
    std::string msg = "AIOS   " + dec(W) + "x" + dec(H) + " is too small for the canvas";
    S.draw_text_to(buf, iw, ih, 12, ih / 2 - 8, msg, kDim, Face::UiSmall);
    finish();
    return;
  }

  // Sample the substrate's OWN telemetry into the sparkline history.
  //
  // These four traces were `20 + 30*sin(t*k + c)` — four phase-shifted sine
  // waves drawn under the heading "CPU core 0..3". They were animated fiction:
  // they moved constantly whatever the machine was doing, which is both a lie
  // about the load and the thing that makes this face look restless over a
  // still wallpaper. SubstrateTelemetry has no per-core figure to draw either,
  // so "core 0..3" could never have been real. It carries four signals that
  // ARE real, and those are what the four lanes carry now.
  //
  // Sampled on a clock, not per frame: at frame rate the 60-slot history was a
  // two-second window that scrolled faster the better the machine ran.
  if (!st.perf_sampled_ms || now < st.perf_sampled_ms || now - st.perf_sampled_ms >= 250) {
    st.perf_sampled_ms = now;
    st.history_idx = (st.history_idx + 1) % 60;
    const auto tel = intelligence::read_substrate_telemetry();
    const float lanes[4] = {
        tel.cpu_load,            // processors busy
        tel.mem_pressure,        // heap used / heap total
        tel.ctx_switch_rate,     // switches per tick, normalised
        tel.compute_occupancy,   // compute lanes busy
    };
    for (int c = 0; c < 4; ++c) {
      float v = lanes[c] * 100.0f;
      st.cpu_history[c][st.history_idx] = std::clamp(v, 0.0f, 100.0f);
    }
  }
  st.mem_history[st.history_idx] = f.heap_kib > 0 ? (f.heap_kib / 1024.0f) / 16.0f * 100.0f : 25.0f;

  // ---- what is left of the layout this display gets ---------------------------
  // The columns and the tiles' places come from canvas_layout() now. What
  // stays here is the chrome the desk does not own: the header the panels sit
  // under and the pulse lanes along the bottom.
  const int pad = lay.cols == 3 ? 24 : 16;
  const int header_h = 78;
  const bool lanes = ih >= 640 && iw >= 780;
  const int lanes_h = lanes ? 96 : 0;

  // `reserve_right` is how much of the header row the CALLER is about to paint
  // a count into, measured. Without it the title only had to fit the card, so
  // at widths where it did and the badge did not, the two were drawn over each
  // other -- "LEDGER - EVERY DECISION ABOUT AUTHORITY" and "24 ON THE LOG"
  // landed on the same pixels at 1600 wide and read as neither.
  auto card = [&](int x, int y, int cw, int ch, const std::string& label, int reserve_right = 0) {
    S.draw_rect_to(buf, iw, ih, x, y, cw, ch, kCard);
    S.draw_round_outline_to(buf, iw, ih, x, y, cw, ch, 8, kBorder); S.draw_rect_to(buf, iw, ih, x + 1, y + 1, cw - 2, 2, kStrong);
    int label_y = (ch <= 36) ? y + 10 : y + 16;
    const int room = cw - 16 - (reserve_right > 0 ? reserve_right + 12 : 16);
    std::string title = label;
    if (S.label_width(title, 2) > room) {
      const size_t dash = label.find(" - ");
      if (dash != std::string::npos) title = label.substr(0, dash);
    }
    // Even the short form can be wider than what is left. Give up characters
    // rather than pixels that belong to something else.
    while (!title.empty() && S.label_width(title, 2) > room) title.pop_back();
    S.draw_label_to(buf, iw, ih, x + 16, label_y, title, kFaint, 2);
  };
  auto bar = [&](int x, int y, int bw, int bh, double fraction, uint32_t colour) {
    S.draw_rect_to(buf, iw, ih, x, y, bw, bh, kPanel);
    int filled = (int)(bw * (fraction < 0 ? 0 : fraction > 1 ? 1 : fraction));
    if (filled > 0) S.draw_rect_to(buf, iw, ih, x, y, filled, bh, colour);
  };
  auto fit = [&](std::string t, int room, Face face) {
    return ::aios::userland::fit(S, t, room, face);
  };
  // Is the pointer on this rect? The desk has rows that ACT — revoke a
  // capability, put the machine back to a moment, open an application, look at
  // an identity — and they were drawn exactly as dead as the rows that do
  // nothing, so the only way to find out which was which was to click and see.
  // In paint coordinates, like everything else the painter lays out.
  const int hot_x = S.mouse_x() / (int)s, hot_y = S.mouse_y() / (int)s;
  auto hot_row = [&](int rx, int ry, int rw, int rh) {
    return hot_x >= rx && hot_x < rx + rw && hot_y >= ry && hot_y < ry + rh;
  };
  // Cleared once a frame rather than by the panel that fills it: the identities
  // panel and the authority panel both put rows in it, and whichever drew first
  // used to have its rows wiped by the other.
  st.identity_rows.clear();
  // Prose wrapped to a column, one line at a time; returns the y after it.
  auto wrap = [&](const std::string& raw, int x, int y, int room, int bottom, Face face, uint32_t first, uint32_t rest) {
    if (room <= 10) return y;
    const std::string text = ascii(raw);
    int line = S.line_height(face);
    std::string cur;
    int cur_w = 0;
    const int space_w = S.text_width(" ", face);
    bool first_line = true;
    size_t p = 0;
    while (p <= text.size() && y + line <= bottom) {
      size_t sp = text.find(' ', p);
      std::string word = text.substr(p, sp == std::string::npos ? std::string::npos : sp - p);
      // The word measured once and the line's width carried, rather than the
      // whole accumulated line re-measured for every word.
      const int word_w = S.text_width(word, face);
      const int cand_w = cur.empty() ? word_w : cur_w + space_w + word_w;
      std::string trial = cur.empty() ? word : cur + " " + word;
      if (cur.empty() || cand_w <= room) { cur = trial; cur_w = cand_w; }
      else {
        S.draw_text_to(buf, iw, ih, x, y, fit(cur, room, face), first_line ? first : rest, face);
        y += line;
        cur = word;
        cur_w = word_w;
        first_line = false;
      }
      if (sp == std::string::npos) break;
      p = sp + 1;
    }
    if (!cur.empty() && y + line <= bottom) {
      S.draw_text_to(buf, iw, ih, x, y, fit(cur, room, face), first_line ? first : rest, face);
      y += line;
    }
    return y;
  };

  // ---- header -----------------------------------------------------------------
  S.draw_rect_to(buf, iw, ih, 0, 0, iw, header_h, kPanel);
  S.draw_rect_to(buf, iw, ih, 0, header_h - 2, iw, 2, kMint);
  S.draw_text_to(buf, iw, ih, pad, 18, "AIOS", kText, Face::UiBold);
  {
    auto t = std::time(nullptr);
    auto tm = *std::localtime(&t);
    char time_str[32];
    std::strftime(time_str, sizeof(time_str), "%a %b %d   %H:%M:%S", &tm);
    std::string header_left = "KERNEL " + k.self().short_str() + "   CONSOLE " + console.self().short_str();
    S.draw_label_to(buf, iw, ih, pad, 48, header_left, kDim, 2);

    std::string right = f.right_label;
    if (right.empty()) {
      right = std::string(time_str);
      if (f.tick_hz && f.ticks) {
        right += "   " + dec(f.ticks / f.tick_hz) + "S UP   " + dec(f.switches) + " SWITCHES   " + dec(f.heap_kib >> 10) + " MIB HEAP   " +
                dec(f.pages_live) + "/" + dec(f.pages_total) + " USER PAGES";
      }
    }
    if (!right.empty() && S.label_width(header_left, 2) + S.label_width(right, 2) + pad * 3 <= iw)
      S.draw_label_to(buf, iw, ih, iw - pad - S.label_width(right, 2), 48, right, kMint, 2);
  }

  // ---- the cards --------------------------------------------------------------
  auto identities = [&](int x, int y, int cw, int ch) {
    if (ch <= 36) {
      std::string summary = dec(st.ledger.size()) + " MINTED";
      card(x, y, cw, ch, "[+] IDENTITIES - 128 BIT", kTileRight + S.label_width(summary, 2));
      S.draw_label_to(buf, iw, ih, x + cw - kTileRight - S.label_width(summary, 2), y + 10, summary, kMint, 2);
      return;
    }
    card(x, y, cw, ch, "IDENTITIES - 128 BIT - CLICK ONE TO SEE WHAT IT IS");
    int yy = y + 46, line = S.line_height(Face::Mono) + 6;
    for (const auto& m : st.ledger) {
      if (yy > y + ch - line) break;
      // A row, not a line of ink: the identity it prints is the handle to the
      // thing, and the pointer has to be able to SAY so before it is pressed.
      const bool hot = hot_row(x + 12, yy - 3, cw - 24, line);
      const bool open = st.inspect_id == m.full && !st.inspect_id.empty();
      if (hot || open) {
        S.draw_rect_to(buf, iw, ih, x + 12, yy - 3, cw - 24, line, open ? 0xFF16232F : kPanel);
        S.draw_rect_to(buf, iw, ih, x + 12, yy - 3, 2, line, kMint);
      }
      uint32_t colour = (m.seen_ms && now - m.seen_ms < 4000) ? kMint : (hot || open ? kText : kDim);
      S.draw_text_to(buf, iw, ih, x + 16, yy, m.id, colour, Face::Mono);
      // The kind starts past the id as it is actually drawn. It was placed at
      // a guessed +110, and twelve monospace hex digits are wider than that,
      // so the last digit sat under the "C" of CAPABILITY.
      const int kind_x = x + 16 + S.text_width(m.id, Face::Mono) + 12;
      S.draw_label_to(buf, iw, ih, kind_x, yy + 2, ascii(m.kind), kFaint, 1);
      const int note_x = kind_x + S.label_width(ascii(m.kind), 1) + 14;
      if (!m.note.empty() && cw > 360 && note_x < x + cw - 40)
        S.draw_text_to(buf, iw, ih, note_x, yy, fit(m.note, x + cw - 16 - note_x, Face::UiSmall), kFaint, Face::UiSmall);
      st.identity_rows.push_back({x + 12, yy - 3, cw - 24, line, m.full, m.kind});
      yy += line;
    }
    if (st.ledger.empty()) S.draw_text_to(buf, iw, ih, x + 16, yy, "nothing has been minted since this opened", kFaint, Face::UiSmall);
  };
  // ---------------------------------------------------------------------------
  // The ledger: every decision this kernel made about authority, newest first.
  // Not a census of what is held — a record of what happened. Minted, derived
  // from what, asked for, REFUSED and why, revoked. This is the page a person
  // who is accountable for a machine that acts on its own opens first, and
  // nothing else has it, because in every other OS the answer is "root did it".
  // ---------------------------------------------------------------------------
  auto ledger = [&](int x, int y, int cw, int ch) {
    const auto& events = k.authority().events();
    if (ch <= 36) {
      std::string n = dec(events.size()) + " DECISIONS";
      card(x, y, cw, ch, "[+] LEDGER - WHAT THE KERNEL DECIDED", kTileRight + S.label_width(n, 2));
      S.draw_label_to(buf, iw, ih, x + cw - kTileRight - S.label_width(n, 2), y + 10, n, kOk, 2);
      return;
    }
    std::string count = dec(events.size()) + " on the log";
    card(x, y, cw, ch, "LEDGER - EVERY DECISION ABOUT AUTHORITY", 16 + S.label_width(count, 1));
    S.draw_label_to(buf, iw, ih, x + cw - 16 - S.label_width(count, 1), y + 18, count, kFaint, 1);

    int yy = y + 44;
    const int line = S.line_height(Face::UiSmall) + 3;
    // The id column starts past the WIDEST kind name there is, measured, not
    // guessed at 78px — "ATTENUATED" is longer than that and ran straight into
    // the entity beside it ("ATTENUAT4e5f805beb01").
    int kind_w = 0;
    for (int kk = AuthorityEvent::Minted; kk <= AuthorityEvent::Attenuated; ++kk) {
      const int lw = S.label_width(authority_event_name((AuthorityEvent::Kind)kk), 1);
      if (lw > kind_w) kind_w = lw;
    }
    const int id_at = 16 + kind_w + 10;
    // Newest first: what just happened is what a person came to see.
    for (size_t i = events.size(); i-- > 0;) {
      if (yy + line > y + ch - 8) break;
      const auto& e = events[i];
      const char* kind = authority_event_name(e.kind);
      // A refusal is the interesting line on this page, so it is the one that
      // is coloured. Everything else is a grant, and grants are ordinary.
      const bool refusal = e.kind == AuthorityEvent::Refused;
      const bool taken = e.kind == AuthorityEvent::Revoked;
      const uint32_t ink = refusal ? kErr : taken ? kWarn : kDim;
      S.draw_label_to(buf, iw, ih, x + 16, yy + 2, kind, ink, 1);
      int at = x + id_at;
      S.draw_text_to(buf, iw, ih, at, yy, e.subject.short_str(), kText, Face::Mono);
      at += S.text_width("000000000000  ", Face::Mono);
      std::string what = e.resource.str() + "  " + rights_str(e.rights);
      const int room = x + cw - 16 - at;
      if (room > 40) S.draw_text_to(buf, iw, ih, at, yy, fit(what, room, Face::UiSmall), ink, Face::UiSmall);
      yy += line;
      // The kernel's own words about the decision, when it had any. On a
      // refusal this is the whole point of the line above it.
      if (!e.note.empty() && yy + line <= y + ch - 8) {
        S.draw_text_to(buf, iw, ih, x + id_at, yy,
                       fit(ascii(e.note), cw - 16 - id_at, Face::UiSmall), kFaint, Face::UiSmall);
        yy += line;
      }
    }
    if (events.empty()) {
      S.draw_text_to(buf, iw, ih, x + 16, y + 48,
                     "no decisions recorded yet", kFaint, Face::UiSmall);
    }
  };

  // ---------------------------------------------------------------------------
  // The rewind. This machine can checkpoint an entity MID-EXECUTION and restore
  // it with the same 128-bit identity and the same position in the graph it was
  // running — states, memory contents, capabilities, channels and all. That is
  // an answer to the thing people are actually afraid of when they let a mind
  // act on their machine, and until now it was reachable only by typing.
  //
  // Both acts are destructive, so a row is ARMED by one click and taken by a
  // second on the same row.
  // ---------------------------------------------------------------------------
  auto rewind = [&](int x, int y, int cw, int ch) {
    st.rewind_rows.clear();
    // What is on disk, re-read a few times a second at most.
    if (st.snapshots.empty() || now < st.snapshots_checked_ms ||
        now - st.snapshots_checked_ms >= 1000) {
      st.snapshots_checked_ms = now;
      st.snapshots.clear();
      const std::string base = k.config().root + "/checkpoints";
      auto dirs = k.host().list_dir(base);
      if (dirs.ok()) {
        for (const auto& d : *dirs) {
          auto files = k.host().list_dir(base + "/" + d);
          if (!files.ok()) continue;
          for (const auto& fname : *files) {
            if (fname.size() < 5 || fname.substr(fname.size() - 4) != ".aoc") continue;
            CanvasState::Snapshot snap;
            snap.path = base + "/" + d + "/" + fname;
            snap.entity = d.size() >= 12 ? d.substr(0, 12) : d;
            snap.ordinal = atoi(fname.c_str());
            auto info = k.host().stat(snap.path);
            if (info.ok()) snap.bytes = info->size;
            // The host boundary reports no modification time, so the ordinal in
            // the file name is the order: the kernel writes 1.aoc, 2.aoc, ...
            snap.taken_ns = (uint64_t)snap.ordinal;
            st.snapshots.push_back(snap);
          }
        }
      }
      // Newest first, so the moment to go back to is the one at the top.
      std::sort(st.snapshots.begin(), st.snapshots.end(),
                [](const CanvasState::Snapshot& a, const CanvasState::Snapshot& b) {
                  if (a.entity != b.entity) return a.entity < b.entity;
                  return a.ordinal > b.ordinal;
                });
      // Whose it is, if that entity is still here.
      for (auto& snap : st.snapshots) {
        for (const auto& e : k.entities().list()) {
          if (e.id.short_str() == snap.entity) { snap.label = e.label; break; }
        }
      }
    }

    if (ch <= 36) {
      std::string n = dec(st.snapshots.size()) + " SNAPSHOTS";
      card(x, y, cw, ch, "[+] REWIND - PUT THE MACHINE BACK", kTileRight + S.label_width(n, 2));
      S.draw_label_to(buf, iw, ih, x + cw - kTileRight - S.label_width(n, 2), y + 10, n, kOk, 2);
      return;
    }
    card(x, y, cw, ch, "REWIND - A MOMENT THIS MACHINE CAN GO BACK TO");

    int yy = y + 44;
    const int row_h = S.line_height(Face::UiSmall) + 14;

    // Taking one is not destructive, so it needs no arming.
    for (const auto& e : k.entities().list()) {
      if (e.label != "mind") continue;
      if (yy + row_h > y + ch - 8) break;
      const std::string subj = "take:" + e.id.str();
      const bool hot = st.armed == subj || hot_row(x + 12, yy, cw - 24, row_h - 4);
      S.draw_rect_to(buf, iw, ih, x + 12, yy, cw - 24, row_h - 4, hot ? kPanel : kCard);
      S.draw_round_outline_to(buf, iw, ih, x + 12, yy, cw - 24, row_h - 4, 6, kBorder);
      S.draw_text_to(buf, iw, ih, x + 22, yy + 5, "checkpoint the mind now", kText, Face::UiSmall);
      std::string held = dec(e.states.size()) + " state(s), " + dec(e.memory.size()) + " memory";
      S.draw_label_to(buf, iw, ih, x + cw - 22 - S.label_width(held, 1), yy + 7, held, kFaint, 1);
      st.rewind_rows.push_back({x + 12, yy, cw - 24, row_h - 4, subj, "checkpoint"});
      yy += row_h;
      break;
    }

    for (const auto& snap : st.snapshots) {
      if (yy + row_h > y + ch - 8) break;
      const bool armed = st.armed == snap.path;
      const bool over = !armed && hot_row(x + 12, yy, cw - 24, row_h - 4);
      S.draw_rect_to(buf, iw, ih, x + 12, yy, cw - 24, row_h - 4, armed ? 0xFF3A1D1D : (over ? kPanel : kCard));
      S.draw_round_outline_to(buf, iw, ih, x + 12, yy, cw - 24, row_h - 4, 6, armed ? kErr : kBorder);
      std::string left = "#" + dec((uint64_t)snap.ordinal) + "  " + snap.entity +
                         (snap.label.empty() ? std::string() : "  " + ascii(snap.label));
      S.draw_text_to(buf, iw, ih, x + 22, yy + 5, left, armed ? kErr : kText, Face::UiSmall);
      if (over) S.draw_rect_to(buf, iw, ih, x + 12, yy, 2, row_h - 4, kMint);
      // Armed, the row says exactly what the second click will do to this
      // machine, in the order it will happen.
      const std::string right = armed ? "click again: terminate, then restore"
                               : over ? "click to put this machine back"
                                      : (dec(snap.bytes / 1024) + " KiB");
      const int rw = S.label_width(right, 1);
      if (rw < cw - 60) {
        S.draw_label_to(buf, iw, ih, x + cw - 22 - rw, yy + 7, right, armed ? kErr : kFaint, 1);
      }
      st.rewind_rows.push_back({x + 12, yy, cw - 24, row_h - 4, snap.path, "restore"});
      yy += row_h;
    }

    if (st.snapshots.empty()) {
      S.draw_text_to(buf, iw, ih, x + 16, yy + 4,
                     "no snapshots yet - take one above", kFaint, Face::UiSmall);
    }
  };

  auto authority = [&](int x, int y, int cw, int ch) {
    if (ch <= 36) {
      std::string summary = dec(k.entities().list().size()) + " ENTITIES";
      card(x, y, cw, ch, "[+] AUTHORITY - CAPABILITY BOUNDS", kTileRight + S.label_width(summary, 2));
      S.draw_label_to(buf, iw, ih, x + cw - kTileRight - S.label_width(summary, 2), y + 10, summary, kOk, 2);
      return;
    }
    card(x, y, cw, ch, "AUTHORITY - WHAT EACH ENTITY MAY DO, AND MAY STOP DOING");
    st.revoke_rows.clear();
    int yy = y + 46;
    const int line = S.line_height(Face::UiSmall) + 4;
    for (const auto& e : k.entities().list()) {
      if (yy + line > y + ch - 8) break;
      auto caps = k.capabilities().list(e.id);
      // The entity itself is a handle: this row opens what it is, what it may
      // do, who granted that, and what it has produced.
      const bool ent_hot = hot_row(x + 12, yy - 2, cw - 24, line);
      const bool ent_open = st.inspect_id == e.id.str();
      if (ent_hot || ent_open) {
        S.draw_rect_to(buf, iw, ih, x + 12, yy - 2, cw - 24, line, ent_open ? 0xFF16232F : kPanel);
        S.draw_rect_to(buf, iw, ih, x + 12, yy - 2, 2, line, kMint);
      }
      st.identity_rows.push_back({x + 12, yy - 2, cw - 24, line, e.id.str(), "entity"});
      const std::string short_id = e.id.short_str();
      S.draw_text_to(buf, iw, ih, x + 16, yy, short_id, ent_hot || ent_open ? kMint : kText, Face::Mono);
      std::string held = dec(caps.size()) + " held";
      const int held_x = x + cw - 16 - S.label_width(held, 1);
      // The label goes after the id as drawn (it sat at a fixed +130, under
      // the end of the id: "23d824c192cfconsole"), and stops short of "held".
      const int label_x = x + 16 + S.text_width(short_id, Face::Mono) + 12;
      if (label_x < held_x - 24)
        S.draw_text_to(buf, iw, ih, label_x, yy, fit(ascii(clip(e.label, 18)), held_x - 12 - label_x, Face::UiSmall),
                       kDim, Face::UiSmall);
      S.draw_label_to(buf, iw, ih, held_x, yy + 2, held, kFaint, 1);
      yy += line;
      // Every capability, and every one of them can be TAKEN AWAY from here.
      // Revoking cascades to everything delegated from it, which is the whole
      // reason this is worth a click: it is the one act that reaches all the
      // way down a delegation chain, and it was only ever reachable by typing.
      for (const auto& cap : caps) {
        if (yy + line > y + ch - 8) break;
        const bool armed = st.armed == cap.id.str();
        // The kernel's own authority is not revocable from here, so it does not
        // light up: a control that cannot act must not look like one.
        const bool kernels = k.self() == e.id;
        const bool hot = !armed && !kernels && hot_row(x + 20, yy - 2, cw - 40, line);
        if (hot) {
          S.draw_rect_to(buf, iw, ih, x + 20, yy - 2, cw - 40, line, kPanel);
          S.draw_rect_to(buf, iw, ih, x + 20, yy - 2, 2, line, kErr);
        }
        if (armed) S.draw_rect_to(buf, iw, ih, x + 20, yy - 2, cw - 40, line, 0xFF3A1D1D);
        // What is on the right decides how much room the rights have, so it is
        // worked out FIRST. The rights used to be clipped to a flat cw - 200
        // while the right-hand label was placed at its own measured width, so
        // any label wider than that guess -- "from 19b92a793349" is -- was
        // painted on top of the end of the rights list.
        std::string right;
        uint32_t ink = kFaint;
        if (armed) { right = "click again to revoke"; ink = kErr; }
        else if (hot) { right = "click to revoke"; ink = kErr; }
        else if (kernels) { right = "the kernel's own"; }
        else if (cap.scope_limit) {
          right = dec(cap.scope_used / 1024) + " / " + dec(cap.scope_limit / 1024) + " KiB";
          const double used = (double)cap.scope_used / (double)cap.scope_limit;
          if (used > 0.85) ink = kErr; else if (used > 0.5) ink = kWarn;
        } else if (!cap.parent.is_null()) {
          right = "from " + cap.parent.short_str();
        }
        const int rw = S.label_width(right, 1);
        const bool show_right = !right.empty() && rw < cw - 220;
        std::string what = cap.resource.str() + "  " + rights_str(cap.rights);
        S.draw_text_to(buf, iw, ih, x + 26, yy,
                       fit(what, cw - 52 - (show_right ? rw + 14 : 0), Face::UiSmall),
                       armed ? kErr : (hot ? kText : kDim), Face::UiSmall);
        if (show_right) {
          S.draw_label_to(buf, iw, ih, x + cw - 26 - rw, yy + 2, right, ink, 1);
        }
        // The kernel's own authority is not a row a person may pull out from
        // under the machine, so it is drawn and not offered.
        if (!kernels) st.revoke_rows.push_back({x + 20, yy - 2, cw - 40, line, cap.id.str(), what});
        yy += line;
      }
      yy += 4;
    }
  };
  auto directives = [&](int x, int y, int cw, int ch) {
    if (ch <= 36) {
      Directives reg(console);
      int cnt = (reg.attach(false).ok()) ? (int)reg.all().size() : 0;
      std::string summary = dec(cnt) + " ACTIVE";
      card(x, y, cw, ch, "[+] STANDING ORDERS", kTileRight + S.label_width(summary, 2));
      S.draw_label_to(buf, iw, ih, x + cw - kTileRight - S.label_width(summary, 2), y + 10, summary, cnt > 0 ? kOk : kDim, 2);
      return;
    }
    card(x, y, cw, ch, "STANDING ORDERS - WORK NOBODY IS TYPING");
    Directives reg(console);
    int yy = y + 46, line = S.line_height(Face::UiSmall) + 10;
    if (reg.attach(false).ok() && !reg.all().empty()) {
      for (const auto& d : reg.all()) {
        if (yy > y + ch - line) break;
        S.draw_text_to(buf, iw, ih, x + 16, yy, ascii(clip(d.name, 16)), kText, Face::UiBold);
        std::string every = "EVERY " + upper(ms_short(d.every_ms)) + "   " + dec(d.runs) + " RUN(S)";
        S.draw_label_to(buf, iw, ih, x + cw - 16 - S.label_width(every, 1), yy + 2, every, kFaint, 1);
        yy += line - 4;
        const uint64_t wall = k.host().wall_ns() / 1000000ull;   // directives keep wall-clock due times
        uint64_t remaining = d.due_ms > wall ? d.due_ms - wall : 0;
        double through = d.every_ms ? 1.0 - (double)remaining / (double)d.every_ms : 1.0;
        bar(x + 16, yy, cw - 32, 5, through, d.last_ok ? kOk : kErr);
        yy += 12;
        S.draw_text_to(buf, iw, ih, x + 16, yy, fit(d.last_result.empty() ? d.line : d.last_result, cw - 32, Face::UiSmall), kFaint, Face::UiSmall);
        yy += line;
      }
    } else {
      S.draw_text_to(buf, iw, ih, x + 16, yy, "none — this machine does nothing unless it is asked", kFaint, Face::UiSmall);
    }
  };
  auto horizon = [&](int x, int y, int cw, int ch) {
    if (ch <= 36) {
      Horizon hz(console);
      std::vector<Signal> sig;
      if (hz.attach(false).ok()) sig = hz.signals(12);
      std::string summary = dec(sig.size()) + " SIGNALS";
      card(x, y, cw, ch, "[+] HORIZON - WORLD SIGNALS", kTileRight + S.label_width(summary, 2));
      S.draw_label_to(buf, iw, ih, x + cw - kTileRight - S.label_width(summary, 2), y + 10, summary, kMint, 2);
      return;
    }
    card(x, y, cw, ch, "HORIZON - WHAT THE WORLD DID THAT TOUCHES THIS MACHINE'S WORK");
    Horizon hz(console);
    std::vector<Signal> sig;
    if (hz.attach(false).ok()) sig = hz.signals(12);
    auto objects = k.store().list();
    std::string sub = sig.empty() ? dec(objects.size()) + " object(s) on the append log, nothing on the horizon yet"
                                  : dec(sig.size()) + " signal(s) of " + dec(objects.size()) + " objects on the append log";
    S.draw_text_to(buf, iw, ih, x + 16, y + 42, fit(sub, cw - 32, Face::UiSmall), kDim, Face::UiSmall);
    int yy = y + 42 + S.line_height(Face::UiSmall) + 12, line = S.line_height(Face::UiSmall);
    double top = 0;
    for (const auto& sg : sig) if (sg.score > top) top = sg.score;
    for (const auto& sg : sig) {
      if (yy > y + ch - line * 2 - 10) break;
      // The bar is what this headline is worth against the best thing here: the
      // shape of the column IS the ranking.
      bar(x + 16, yy + 3, 4, line, top > 0 ? sg.score / top : 0.0, kMint);
      int room = cw - 44;
      S.draw_text_to(buf, iw, ih, x + 28, yy, fit(sg.title, room, Face::UiSmall), kText, Face::UiSmall);
      yy += line + 2;
      std::string why = sg.source;
      for (const auto& m : sg.matched) why += (why == sg.source ? " - " : ", ") + m;
      S.draw_text_to(buf, iw, ih, x + 28, yy, fit(why, room, Face::UiSmall), kFaint, Face::UiSmall);
      yy += line + 8;
    }
    if (sig.empty()) S.draw_text_to(buf, iw, ih, x + 16, yy, "nothing out there has touched this machine's own work", kFaint, Face::UiSmall);
  };
  auto brief = [&](int x, int y, int cw, int ch) {
    if (ch <= 36) {
      std::string summary = "1 BRIEF";
      card(x, y, cw, ch, "[+] THE BRIEF - MIND DISTILLATION", kTileRight + S.label_width(summary, 2));
      S.draw_label_to(buf, iw, ih, x + cw - kTileRight - S.label_width(summary, 2), y + 10, summary, kViolet, 2);
      return;
    }
    card(x, y, cw, ch, "THE BRIEF - WHAT THE MIND SAYS IT MEANS");
    // The newest brief the mind wrote (kind=horizon-brief), as prose, wrapped.
    StoredObject best;
    bool have = false;
    for (const auto& id : k.store().find_meta("kind", "horizon-brief")) {
      auto o = k.store().get(id);
      if (o.ok() && (!have || o->timestamp_ns > best.timestamp_ns)) { best = *o; have = true; }
    }
    int yy = y + 42, line = S.line_height(Face::UiSmall);
    if (!have) {
      S.draw_text_to(buf, iw, ih, x + 16, yy, "no brief yet — the mind has not distilled the horizon", kFaint, Face::UiSmall);
      return;
    }
    std::string sub = upper(best.meta("date"));
    std::string src = best.meta("sources");
    if (!src.empty()) sub += "   " + src + " SOURCES";
    S.draw_label_to(buf, iw, ih, x + 16, yy, sub, kDim, 1);
    yy += line + 8;
    std::string text(best.payload.begin(), best.payload.end());
    int room = cw - 44, bottom = y + ch - 12;
    size_t at = 0;
    while (at <= text.size() && yy + line <= bottom) {
      size_t sep = text.find(" | ", at);
      std::string item = text.substr(at, sep == std::string::npos ? std::string::npos : sep - at);
      while (!item.empty() && item.front() == ' ') item.erase(item.begin());
      if (!item.empty()) {
        S.draw_rect_to(buf, iw, ih, x + 16, yy + 5, 4, 4, kViolet);
        // A distilled item is "TITLE :: what :: why :: URL": the title in full,
        // then why it touches this machine; the wire detail stays in the store.
        std::vector<std::string> seg;
        const std::string sep4 = " :: ";
        for (size_t p0 = 0;;) { size_t d = item.find(sep4, p0); seg.push_back(item.substr(p0, d == std::string::npos ? std::string::npos : d - p0)); if (d == std::string::npos) break; p0 = d + sep4.size(); }
        if (seg.size() >= 4) {
          yy = wrap(seg[0], x + 28, yy, room, bottom, Face::UiBold, kText, kText) + 2;
          yy = wrap(seg[2], x + 28, yy, room, bottom, Face::UiSmall, kDim, kDim) + 10;
        } else {
          yy = wrap(item, x + 28, yy, room, bottom, Face::UiSmall, kText, kDim) + 6;
        }
      }
      if (sep == std::string::npos) break;
      at = sep + 3;
    }
  };
  auto clock_calendar = [&](int x, int y, int cw, int ch) {
    if (ch <= 36) {
      auto t = std::time(nullptr);
      auto tm = *std::localtime(&t);
      char tbuf[32];
      std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S  %b %d", &tm);
      std::string summary = tbuf;
      card(x, y, cw, ch, "[+] CLOCK & CALENDAR", kTileRight + S.label_width(summary, 2));
      S.draw_label_to(buf, iw, ih, x + cw - kTileRight - S.label_width(summary, 2), y + 10, summary, kMint, 2);
      return;
    }
    card(x, y, cw, ch, "CLOCK & CALENDAR - SYSTEM TIME & MONTHLY GRID");
    auto t = std::time(nullptr);
    auto tm = *std::localtime(&t);
    char time_str[32], date_str[64];
    std::strftime(time_str, sizeof(time_str), "%H:%M:%S", &tm);
    std::strftime(date_str, sizeof(date_str), "%A, %B %d, %Y", &tm);

    // Digital Clock & Date
    S.draw_text_to(buf, iw, ih, x + 16, y + 42, time_str, kMint, Face::UiBold);
    S.draw_text_to(buf, iw, ih, x + 16, y + 66, date_str, kDim, Face::UiSmall);

    // Month Header
    char month_hdr[32];
    std::strftime(month_hdr, sizeof(month_hdr), "%B %Y", &tm);
    int cal_y = y + 90;
    S.draw_label_to(buf, iw, ih, x + 16, cal_y, upper(month_hdr), kText, 2);
    cal_y += 18;

    // Day headers
    const char* days[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
    int cell_w = (cw - 32) / 7;
    for (int i = 0; i < 7; ++i) {
      S.draw_label_to(buf, iw, ih, x + 16 + i * cell_w, cal_y, days[i], kFaint, 1);
    }
    cal_y += 16;

    int year = tm.tm_year + 1900;
    int month = tm.tm_mon;
    int mday = tm.tm_mday;

    std::tm first_tm = {};
    first_tm.tm_year = tm.tm_year;
    first_tm.tm_mon = tm.tm_mon;
    first_tm.tm_mday = 1;
    std::mktime(&first_tm);
    int first_dow = first_tm.tm_wday;

    int days_in_month = 31;
    if (month == 3 || month == 5 || month == 8 || month == 10) days_in_month = 30;
    else if (month == 1) {
      bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
      days_in_month = leap ? 29 : 28;
    }

    int cur_col = first_dow;
    int cur_row = 0;
    for (int d = 1; d <= days_in_month; ++d) {
      int dx = x + 16 + cur_col * cell_w;
      int dy = cal_y + cur_row * 18;
      if (dy + 16 > y + ch) break;
      std::string dstr = (d < 10 ? " " : "") + dec(d);
      bool is_today = (d == mday);
      if (is_today) {
        S.draw_rect_to(buf, iw, ih, dx - 2, dy - 2, 22, 16, kMint);
        S.draw_text_to(buf, iw, ih, dx + 2, dy, dec(d), kPanel, Face::UiBold);
      } else {
        S.draw_text_to(buf, iw, ih, dx, dy, dstr, kText, Face::UiSmall);
      }
      cur_col++;
      if (cur_col >= 7) { cur_col = 0; cur_row++; }
    }
  };

  // Sparkline Performance & Telemetry Renderer
  auto sparklines_perf = [&](int x, int y, int cw, int ch) {
    if (ch <= 40) {
      // The collapsed summary is the newest sample the open chart draws, not a
      // number typed into the source.
      float cpu_now = st.cpu_history[0][st.history_idx];
      std::string summary = "CPU " + dec((uint64_t)(cpu_now < 0 ? 0 : cpu_now)) + "%  RAM " + dec(f.heap_kib / 1024) + "MB";
      card(x, y, cw, ch, "SUBSTRATE - CPU . MEMORY . SWITCHES . COMPUTE", kTileRight + S.label_width(summary, 2));
      S.draw_label_to(buf, iw, ih, x + cw - kTileRight - S.label_width(summary, 2), y + 10, summary, kOk, 2);
      return;
    }
    card(x, y, cw, ch, "SUBSTRATE - CPU . MEMORY . SWITCHES . COMPUTE");
    int graph_y = y + 36;
    int graph_h = ch - 48;
    int graph_w = cw - 32;

    // Background chart gridlines
    for (int g = 0; g <= 4; ++g) {
      int gy = graph_y + g * (graph_h / 4);
      S.draw_rect_to(buf, iw, ih, x + 16, gy, graph_w, 1, 0xFF1C1C1C);
    }

    // Draw CPU core 0..3 sparkline bars / paths
    // One colour per real signal, in the order they are sampled above.
    uint32_t core_colors[4] = {kMint, kOk, kWarn, kViolet};
    for (int c = 0; c < 4; ++c) {
      for (int i = 0; i < 59; ++i) {
        int idx1 = (st.history_idx + i) % 60;
        int idx2 = (st.history_idx + i + 1) % 60;
        float v1 = st.cpu_history[c][idx1];
        float v2 = st.cpu_history[c][idx2];

        int px1 = x + 16 + i * graph_w / 60;
        int px2 = x + 16 + (i + 1) * graph_w / 60;
        int py1 = graph_y + graph_h - static_cast<int>(v1 * graph_h / 100.0f);
        int py2 = graph_y + graph_h - static_cast<int>(v2 * graph_h / 100.0f);
        py1 = std::clamp(py1, graph_y, graph_y + graph_h);
        py2 = std::clamp(py2, graph_y, graph_y + graph_h);

        { int seg_top = std::min(py1, py2); int seg_h = std::max(2, (py1 > py2 ? py1 - py2 : py2 - py1) + 2); S.draw_rect_to(buf, iw, ih, px1, seg_top, std::max(1, px2 - px1), seg_h, core_colors[c]); }
      }
    }

    std::string legend = "C0:MINT  C1:OK  C2:WARN  C3:VIOLET";
    S.draw_label_to(buf, iw, ih, x + 16, y + ch - 16, legend, kDim, 1);
  };

  // ---- the desk ---------------------------------------------------------------
  // Where these seven panels sit is a person's business now, not a per-column
  // if/else. What used to be here was three hardcoded arrangements — one each
  // for a three-, two- and one-column display — plus an all-or-nothing
  // "collapsed" variant of each, and no panel could be moved or thrown out.
  for (const TileRect& r : lay.tiles) {
    if (r.index < 0 || r.index >= (int)st.tiles.size()) continue;
    const bool folded = r.h <= kFoldH;
    switch (r.kind) {
      case TileKind::ClockCalendar: clock_calendar(r.x, r.y, r.w, r.h); break;
      case TileKind::Performance:   sparklines_perf(r.x, r.y, r.w, r.h); break;
      case TileKind::Identities:    identities(r.x, r.y, r.w, r.h); break;
      case TileKind::Authority:     authority(r.x, r.y, r.w, r.h); break;
      case TileKind::Directives:    directives(r.x, r.y, r.w, r.h); break;
      case TileKind::Horizon:       horizon(r.x, r.y, r.w, r.h); break;
      case TileKind::Brief:         brief(r.x, r.y, r.w, r.h); break;
      case TileKind::Ledger:        ledger(r.x, r.y, r.w, r.h); break;
      case TileKind::Rewind:        rewind(r.x, r.y, r.w, r.h); break;
      case TileKind::ThoughtStream: draw_log_tile(S, buf, iw, ih, k, st, now, r.x, r.y, r.w, r.h); break;
      case TileKind::Apps:          draw_apps_tile(S, buf, iw, ih, k, st, now, r.x, r.y, r.w, r.h); break;
      case TileKind::MindAnswer:    draw_answer_tile(S, buf, iw, ih, st, now, r.x, r.y, r.w, r.h); break;
      case TileKind::AppPane:       S.paint_app_tile(buf, iw, ih, k, r, lay.scale); break;
      case TileKind::Inspect:       draw_inspect_tile(S, buf, iw, ih, k, st, now, r.x, r.y, r.w, r.h); break;
      default: break;
    }
    draw_tile_chrome(S, buf, iw, ih, r, folded, kFaint);
  }

  // ---- lanes: the machine's pulse ---------------------------------------------
  if (lanes) {
    int lane_y = ih - lanes_h;
    S.draw_rect_to(buf, iw, ih, 0, lane_y, iw, lanes_h, kPanel);
    S.draw_rect_to(buf, iw, ih, 0, lane_y, iw, 2, kMint);
    struct Lane { const char* name; uint64_t value; uint32_t colour; };
    Lane all[] = {
        {"IDENTITIES", (uint64_t)st.known_ids.size(), kMint},
        {"OBJECTS", [&]{ static uint64_t v = 0, at = ~0ull; if (at == ~0ull || st.frame - at >= 60) { v = (uint64_t)k.store().list().size(); at = st.frame; } return v; }(), kOk},
        {"SIGNALS", [&]{ static uint64_t v = 0, at = ~0ull; if (at == ~0ull || st.frame - at >= 60) { v = (uint64_t)k.store().find_meta("kind", "signal").size(); at = st.frame; } return v; }(), kMint},
        {"ENTITIES", (uint64_t)k.entities().list().size(), kOk},
        {"STATES", (uint64_t)k.states().list().size(), kDim},
        {"TASKS", (uint64_t)f.tasks.size(), kDim},
        {"FRAMES IN", f.rx_frames, f.net_up ? kOk : kFaint},
        {"FRAMES OUT", f.tx_frames, f.net_up ? kOk : kFaint},
    };
    int n = iw >= 1150 ? 8 : 5;   // fewer lanes on a narrower display, never a squeezed one
    int lane_w = (iw - pad * 2) / n;
    for (int i = 0; i < n; ++i) {
      int lx = pad + lane_w * i;
      S.draw_label_to(buf, iw, ih, lx, lane_y + 22, all[i].name, kFaint, 2);
      S.draw_text_to(buf, iw, ih, lx, lane_y + 40, dec(all[i].value), kText, Face::UiBold);
      int track = lane_w - 24;
      int pulse = track > 20 ? (int)((st.frame * 7 + (uint64_t)i * 23) % (uint64_t)(track - 18)) : 0;
      S.draw_rect_to(buf, iw, ih, lx, lane_y + lanes_h - 22, track, 3, kBorder);
      S.draw_rect_to(buf, iw, ih, lx + pulse, lane_y + lanes_h - 22, 18, 3, all[i].colour);
    }
  }
  finish();
}

#if !defined(AIOS_METAL)
bool write_ppm(const std::string& path, const std::vector<uint32_t>& argb, uint32_t w, uint32_t h) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f << "P6\n" << w << " " << h << "\n255\n";
  std::vector<unsigned char> row(w * 3);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      uint32_t p = argb[(size_t)y * w + x];
      row[x * 3] = (unsigned char)(p >> 16); row[x * 3 + 1] = (unsigned char)(p >> 8); row[x * 3 + 2] = (unsigned char)p;
    }
    f.write((const char*)row.data(), row.size());
  }
  return (bool)f;
}
#endif

}  // namespace aios::userland
