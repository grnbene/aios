#include <chrono>
// The stylesheet parser, the default sheet, and the cascade.
//
// This is a real cascade, not a lookup table of tag names: rules are matched by
// selector, weighted by specificity, broken by document order, and then the
// element's own `style=""` goes on top. `@media` queries are kept on the rule
// and evaluated against the viewport at cascade time, so the same parsed sheet
// reflows when the window is resized instead of being re-parsed.
//
// Selectors: tag, id, class, the universal, `>` `+` `~` and descendant
// combinators, attribute terms with `= ~= |= ^= $= *=` as well as presence,
// the structural pseudo-classes (:first-child, :last-child, :only-child,
// :nth-child(an+b), :root), and :not() / :is() / :where() with full selector
// lists, evaluated by re-entering the matcher.
//
// What it does NOT do: :has(), which needs a second pass over the subtree --
// a compound containing one matches nothing rather than guessing, because
// guessing "yes" is what over-applied every rule that had one. Pseudo-ELEMENTS
// likewise match nothing: they style generated content no element here has.
//
// The three splitters in here -- selector list on commas, selector on
// combinators, declarations on semicolons -- are all depth- and quote-aware.
// They were not, and every one of them was silently dropping rules: `:is(h1,
// h2)` split into two nonsense selectors at the comma so only the first
// alternative was ever honoured, `[class~="tag"]` split at the tilde, and
// `[title="a b"]` split at the space.
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <memory>
#include <unordered_map>

#include "aios/userland/web.hpp"
#include <cstdlib>
#include <cstdio>

bool g_css_has_hover = false;   // some stylesheet had a :hover rule (read by web_pane.cpp)

namespace aios::userland::web {
namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

std::string lowered(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) o += lower(c);
  return o;
}

std::string trimmed(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && is_space(s[a])) ++a;
  while (b > a && is_space(s[b - 1])) --b;
  return s.substr(a, b - a);
}

bool starts_with(const std::string& s, const char* what) {
  size_t n = std::strlen(what);
  return s.size() >= n && std::memcmp(s.data(), what, n) == 0;
}

// ------------------------------------------------------------------ values --

// A number, with however much of a fraction it has. No strtod: this compiles
// into a freestanding kernel, and a locale-aware parse of "1.5" is exactly the
// kind of thing that is not there.
bool parse_number(const std::string& s, size_t& i, float& out) {
  size_t start = i;
  bool neg = false;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
    neg = s[i] == '-';
    ++i;
  }
  float whole = 0;
  size_t digits = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    whole = whole * 10 + (float)(s[i] - '0');
    ++i;
    ++digits;
  }
  float frac = 0, scale = 1;
  if (i < s.size() && s[i] == '.') {
    ++i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      scale *= 0.1f;
      frac += (float)(s[i] - '0') * scale;
      ++i;
      ++digits;
    }
  }
  if (!digits) {
    i = start;
    return false;
  }
  out = (whole + frac) * (neg ? -1.0f : 1.0f);
  return true;
}

// An angle as radians: `45deg`, `0.5turn`, `1.2rad`, `100grad`, or a bare 0.
bool parse_angle(const std::string& s, float& out) {
  size_t i = 0;
  float n = 0;
  while (i < s.size() && s[i] == ' ') ++i;
  if (!parse_number(s, i, n)) return false;
  std::string unit = s.substr(i);
  while (!unit.empty() && unit.back() == ' ') unit.pop_back();
  const float pi = 3.14159265358979f;
  if (unit == "deg") out = n * pi / 180.0f;
  else if (unit == "rad") out = n;
  else if (unit == "grad") out = n * pi / 200.0f;
  else if (unit == "turn") out = n * 2.0f * pi;
  else if (unit.empty() && n == 0) out = 0;
  else return false;
  return true;
}

// A scale: a number, or a percentage of one.
float scale_factor(const std::string& s) {
  size_t i = 0;
  float n = 1;
  if (!parse_number(s, i, n)) return 1.0f;
  return i < s.size() && s[i] == '%' ? n / 100.0f : n;
}

int round_to_int(float v) { return (int)(v < 0 ? v - 0.5f : v + 0.5f); }

struct NamedColour {
  const char* name;
  uint32_t argb;
};

// The colours a page names instead of writing. Not all 148: the ones that
// actually turn up, plus every one the default sheet uses.
constexpr NamedColour kColours[] = {
    {"black", 0xFF000000},   {"white", 0xFFFFFFFF},   {"red", 0xFFFF0000},     {"lime", 0xFF00FF00},
    {"blue", 0xFF0000FF},    {"yellow", 0xFFFFFF00},  {"cyan", 0xFF00FFFF},    {"aqua", 0xFF00FFFF},
    {"magenta", 0xFFFF00FF}, {"fuchsia", 0xFFFF00FF}, {"silver", 0xFFC0C0C0},  {"gray", 0xFF808080},
    {"grey", 0xFF808080},    {"maroon", 0xFF800000},  {"olive", 0xFF808000},   {"green", 0xFF008000},
    {"purple", 0xFF800080},  {"teal", 0xFF008080},    {"navy", 0xFF000080},    {"orange", 0xFFFFA500},
    {"gold", 0xFFFFD700},    {"pink", 0xFFFFC0CB},    {"brown", 0xFFA52A2A},   {"beige", 0xFFF5F5DC},
    {"ivory", 0xFFFFFFF0},   {"khaki", 0xFFF0E68C},   {"salmon", 0xFFFA8072},  {"crimson", 0xFFDC143C},
    {"indigo", 0xFF4B0082},  {"violet", 0xFFEE82EE},  {"orchid", 0xFFDA70D6},  {"plum", 0xFFDDA0DD},
    {"tan", 0xFFD2B48C},     {"coral", 0xFFFF7F50},   {"tomato", 0xFFFF6347},  {"turquoise", 0xFF40E0D0},
    {"skyblue", 0xFF87CEEB},         {"steelblue", 0xFF4682B4},   {"royalblue", 0xFF4169E1},
    {"dodgerblue", 0xFF1E90FF},      {"deepskyblue", 0xFF00BFFF}, {"cornflowerblue", 0xFF6495ED},
    {"midnightblue", 0xFF191970},    {"slateblue", 0xFF6A5ACD},   {"slategray", 0xFF708090},
    {"slategrey", 0xFF708090},       {"lightgray", 0xFFD3D3D3},   {"lightgrey", 0xFFD3D3D3},
    {"darkgray", 0xFFA9A9A9},        {"darkgrey", 0xFFA9A9A9},    {"dimgray", 0xFF696969},
    {"dimgrey", 0xFF696969},         {"whitesmoke", 0xFFF5F5F5},  {"gainsboro", 0xFFDCDCDC},
    {"seagreen", 0xFF2E8B57},        {"forestgreen", 0xFF228B22}, {"limegreen", 0xFF32CD32},
    {"darkgreen", 0xFF006400},       {"lightgreen", 0xFF90EE90},  {"darkred", 0xFF8B0000},
    {"firebrick", 0xFFB22222},       {"darkorange", 0xFFFF8C00},  {"goldenrod", 0xFFDAA520},
    {"chocolate", 0xFFD2691E},       {"sienna", 0xFFA0522D},      {"peru", 0xFFCD853F},
    {"lavender", 0xFFE6E6FA},        {"linen", 0xFFFAF0E6},       {"snow", 0xFFFFFAFA},
    {"azure", 0xFFF0FFFF},           {"aliceblue", 0xFFF0F8FF},   {"transparent", 0x00000000},
     {"lightblue", 0xFFADD8E6},       {"lightyellow", 0xFFFFFFE0}, {"lightpink", 0xFFFFB6C1},
     {"darkblue", 0xFF00008B},        {"rebeccapurple", 0xFF663399},
};

bool hex_nibble(char c, uint32_t& v) {
  if (c >= '0' && c <= '9') { v = (uint32_t)(c - '0'); return true; }
  char l = lower(c);
  if (l >= 'a' && l <= 'f') { v = (uint32_t)(l - 'a' + 10); return true; }
  return false;
}

bool parse_colour(const std::string& raw, uint32_t& out);

// Modern colour functions: hwb(), lab(), lch(), oklab(), oklch(), color-mix().
// Everything is converted to sRGB8 here because the painter only knows ARGB.
// `v` is already lowered and trimmed. Relative colours (`from ...`) are not
// supported and fail the parse, so the declaration is dropped like any
// unknown value.
bool parse_colour_modern(const std::string& v, uint32_t& out) {
  size_t open = v.find('(');
  if (open == std::string::npos || v.back() != ')') return false;
  std::string fn = v.substr(0, open);
  std::string inner = v.substr(open + 1, v.size() - open - 2);
  auto to8 = [](float f) {
    f = std::max(0.0f, std::min(1.0f, f));
    return (uint32_t)(f * 255.0f + 0.5f);
  };
  auto gamma = [](float c) {
    float a = std::fabs(c);
    float r = a <= 0.0031308f ? 12.92f * a : 1.055f * std::pow(a, 1.0f / 2.4f) - 0.055f;
    return c < 0 ? -r : r;
  };
  auto degamma = [](float c) {
    float a = std::fabs(c);
    float r = a <= 0.04045f ? a / 12.92f : std::pow((a + 0.055f) / 1.055f, 2.4f);
    return c < 0 ? -r : r;
  };
  if (fn == "color-mix") {
    // color-mix(in <space>, <colour> [p%], <colour> [p%])
    std::vector<std::string> parts;
    int depth = 0;
    std::string cur;
    for (char ch : inner) {
      if (ch == '(') depth++;
      if (ch == ')') depth--;
      if (ch == ',' && depth == 0) { parts.push_back(cur); cur.clear(); continue; }
      cur += ch;
    }
    parts.push_back(cur);
    if (parts.size() != 3) return false;
    std::string space = trimmed(parts[0]);
    if (space.compare(0, 3, "in ") != 0) return false;
    uint32_t col[2];
    float pct[2] = {-1, -1};
    for (int k = 0; k < 2; ++k) {
      std::string s = trimmed(parts[k + 1]);
      size_t pp = s.rfind(' ');
      // A trailing `NN%` is the weight; the colour is whatever precedes it.
      if (pp != std::string::npos && s.back() == '%') {
        pct[k] = (float)std::atof(s.substr(pp + 1).c_str());
        s = trimmed(s.substr(0, pp));
      } else if (!s.empty() && s[0] >= '0' && s[0] <= '9' && pp != std::string::npos) {
        pct[k] = (float)std::atof(s.c_str());
        s = trimmed(s.substr(pp + 1));
      }
      if (!parse_colour(s, col[k])) return false;
    }
    if (pct[0] < 0 && pct[1] < 0) { pct[0] = 50; pct[1] = 50; }
    else if (pct[0] < 0) pct[0] = 100 - pct[1];
    else if (pct[1] < 0) pct[1] = 100 - pct[0];
    float sum = pct[0] + pct[1];
    if (sum <= 0) return false;
    float w0 = pct[0] / sum, w1 = pct[1] / sum;
    float alpha_mul = sum < 100 ? sum / 100.0f : 1.0f;
    float a0 = ((col[0] >> 24) & 255) / 255.0f, a1 = ((col[1] >> 24) & 255) / 255.0f;
    float a = a0 * w0 + a1 * w1;
    bool linear = space.find("linear") != std::string::npos;
    float res[3];
    if (space.find("lab") != std::string::npos || space.find("lch") != std::string::npos) {
      // Perceptual spaces: mix in OKLab (lab/lch are approximated by it; hue
      // interpolation is not modelled, which only matters for lch/oklch).
      float lab[2][3];
      for (int k = 0; k < 2; ++k) {
        float r = degamma(((col[k] >> 16) & 255) / 255.0f), g = degamma(((col[k] >> 8) & 255) / 255.0f),
              b = degamma((col[k] & 255) / 255.0f);
        float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
        float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
        float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
        lab[k][0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
        lab[k][1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
        lab[k][2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
      }
      float L = lab[0][0] * w0 + lab[1][0] * w1, A = lab[0][1] * w0 + lab[1][1] * w1,
            B = lab[0][2] * w0 + lab[1][2] * w1;
      float l_ = L + 0.3963377774f * A + 0.2158037573f * B;
      float m_ = L - 0.1055613458f * A - 0.0638541728f * B;
      float s_ = L - 0.0894841775f * A - 1.2914855480f * B;
      float l3 = l_ * l_ * l_, m3 = m_ * m_ * m_, s3 = s_ * s_ * s_;
      res[0] = gamma(4.0767416621f * l3 - 3.3077115913f * m3 + 0.2309699292f * s3);
      res[1] = gamma(-1.2684380046f * l3 + 2.6097574011f * m3 - 0.3413193965f * s3);
      res[2] = gamma(-0.0041960863f * l3 - 0.7034186147f * m3 + 1.7076147010f * s3);
      out = (to8(a * alpha_mul) << 24) | (to8(res[0]) << 16) | (to8(res[1]) << 8) | to8(res[2]);
      return true;
    }
    for (int c = 0; c < 3; ++c) {
      float x0 = ((col[0] >> (16 - 8 * c)) & 255) / 255.0f;
      float x1 = ((col[1] >> (16 - 8 * c)) & 255) / 255.0f;
      if (linear) { x0 = degamma(x0); x1 = degamma(x1); }
      // Premultiplied mix, as the spec does for non-hue spaces.
      res[c] = a > 0 ? (x0 * a0 * w0 + x1 * a1 * w1) / a : 0.0f;
      if (linear) res[c] = gamma(res[c]);
    }
    out = (to8(a * alpha_mul) << 24) | (to8(res[0]) << 16) | (to8(res[1]) << 8) | to8(res[2]);
    return true;
  }
  // Split the arguments on space, comma and slash.
  std::vector<std::string> tok;
  {
    std::string cur;
    for (char ch : inner) {
      if (ch == ' ' || ch == ',' || ch == '/' || ch == '\t') {
        if (!cur.empty()) { tok.push_back(cur); cur.clear(); }
      } else cur += ch;
    }
    if (!cur.empty()) tok.push_back(cur);
  }
  if (tok.size() < 3 || tok.size() > 4 || tok[0] == "from") return false;
  float val[4] = {0, 0, 0, 1};
  bool pc[4] = {false, false, false, false};
  for (size_t k = 0; k < tok.size(); ++k) {
    std::string t = tok[k];
    if (t == "none") { val[k] = 0; continue; }
    float scale = 1.0f;
    if (t.back() == '%') { pc[k] = true; t.pop_back(); }
    else if (t.size() > 3 && t.compare(t.size() - 3, 3, "deg") == 0) t.resize(t.size() - 3);
    else if (t.size() > 4 && t.compare(t.size() - 4, 4, "turn") == 0) { t.resize(t.size() - 4); scale = 360.0f; }
    else if (t.size() > 3 && t.compare(t.size() - 3, 3, "rad") == 0) { t.resize(t.size() - 3); scale = 57.29578f; }
    char* end = nullptr;
    float f = std::strtof(t.c_str(), &end);
    if (end == t.c_str() || *end) return false;
    val[k] = f * scale;
  }
  float alpha = tok.size() == 4 ? (pc[3] ? val[3] / 100.0f : val[3]) : 1.0f;
  float rgb[3];
  auto hue_unit = [](float h) {
    h = std::fmod(h, 360.0f);
    return h < 0 ? h + 360.0f : h;
  };
  if (fn == "hwb") {
    float h = hue_unit(val[0]) / 60.0f;
    float w = std::max(0.0f, val[1] / 100.0f), b = std::max(0.0f, val[2] / 100.0f);
    if (w + b >= 1.0f) { float g = w / (w + b); rgb[0] = rgb[1] = rgb[2] = g; }
    else {
      float base[3];
      for (int c = 0; c < 3; ++c) {
        float x = std::fabs(std::fmod(h + (c == 0 ? 0.0f : c == 1 ? 4.0f : 2.0f), 6.0f) - 3.0f) - 1.0f;
        base[c] = std::max(0.0f, std::min(1.0f, x));
        rgb[c] = base[c] * (1.0f - w - b) + w;
      }
    }
  } else if (fn == "oklab" || fn == "oklch") {
    float L = pc[0] ? val[0] / 100.0f : val[0];
    float A, B;
    if (fn == "oklab") {
      A = pc[1] ? val[1] * 0.4f / 100.0f : val[1];
      B = pc[2] ? val[2] * 0.4f / 100.0f : val[2];
    } else {
      float C = pc[1] ? val[1] * 0.4f / 100.0f : val[1];
      float hr = hue_unit(val[2]) * 3.14159265f / 180.0f;
      A = C * std::cos(hr); B = C * std::sin(hr);
    }
    float l_ = L + 0.3963377774f * A + 0.2158037573f * B;
    float m_ = L - 0.1055613458f * A - 0.0638541728f * B;
    float s_ = L - 0.0894841775f * A - 1.2914855480f * B;
    float l3 = l_ * l_ * l_, m3 = m_ * m_ * m_, s3 = s_ * s_ * s_;
    float lr = 4.0767416621f * l3 - 3.3077115913f * m3 + 0.2309699292f * s3;
    float lg = -1.2684380046f * l3 + 2.6097574011f * m3 - 0.3413193965f * s3;
    float lb = -0.0041960863f * l3 - 0.7034186147f * m3 + 1.7076147010f * s3;
    rgb[0] = gamma(lr); rgb[1] = gamma(lg); rgb[2] = gamma(lb);
  } else if (fn == "lab" || fn == "lch") {
    float L = val[0];
    float A, B;
    if (fn == "lab") {
      A = pc[1] ? val[1] * 1.25f : val[1];
      B = pc[2] ? val[2] * 1.25f : val[2];
    } else {
      float C = pc[1] ? val[1] * 1.5f : val[1];
      float hr = hue_unit(val[2]) * 3.14159265f / 180.0f;
      A = C * std::cos(hr); B = C * std::sin(hr);
    }
    // CIE Lab (D50) -> XYZ -> Bradford to D65 -> linear sRGB.
    float fy = (L + 16.0f) / 116.0f, fx = fy + A / 500.0f, fz = fy - B / 200.0f;
    auto finv = [](float t) {
      return t * t * t > 216.0f / 24389.0f ? t * t * t : (116.0f * t - 16.0f) * 27.0f / 24389.0f;
    };
    float X = finv(fx) * 0.3457f / 0.3585f, Y = L > 8.0f ? fy * fy * fy : L * 27.0f / 24389.0f,
          Z = finv(fz) * (1.0f - 0.3457f - 0.3585f) / 0.3585f;
    float X65 = 0.9554734527f * X - 0.0230985368f * Y + 0.0632593086f * Z;
    float Y65 = -0.0283697069f * X + 1.0099954580f * Y + 0.0210413989f * Z;
    float Z65 = 0.0123140016f * X - 0.0205076964f * Y + 1.3303659366f * Z;
    float lr = 3.2409699419f * X65 - 1.5373831776f * Y65 - 0.4986107603f * Z65;
    float lg = -0.9692436363f * X65 + 1.8759675015f * Y65 + 0.0415550574f * Z65;
    float lb = 0.0556300797f * X65 - 0.2039769589f * Y65 + 1.0569715142f * Z65;
    rgb[0] = gamma(lr); rgb[1] = gamma(lg); rgb[2] = gamma(lb);
  } else {
    return false;
  }
  out = (to8(alpha) << 24) | (to8(rgb[0]) << 16) | (to8(rgb[1]) << 8) | to8(rgb[2]);
  return true;
}

// `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, `rgb()`, `rgba()`, a name, or
// `currentcolor`. Returns false when the value is not a colour at all, which is
// how `background: url(...) no-repeat` avoids painting a black box.
bool parse_colour(const std::string& raw, uint32_t& out) {
  std::string v = trimmed(lowered(raw));
  if (v.empty()) return false;
  if (v[0] == '#') {
    uint32_t n[8];
    size_t count = 0;
    for (size_t i = 1; i < v.size() && count < 8; ++i) {
      if (!hex_nibble(v[i], n[count])) return false;
      ++count;
    }
    auto dup = [](uint32_t x) { return x * 16 + x; };
    if (count == 3) { out = 0xFF000000u | (dup(n[0]) << 16) | (dup(n[1]) << 8) | dup(n[2]); return true; }
    if (count == 4) {
      out = (dup(n[3]) << 24) | (dup(n[0]) << 16) | (dup(n[1]) << 8) | dup(n[2]);
      return true;
    }
    if (count == 6) {
      out = 0xFF000000u | ((n[0] * 16 + n[1]) << 16) | ((n[2] * 16 + n[3]) << 8) | (n[4] * 16 + n[5]);
      return true;
    }
    if (count == 8) {
      out = ((n[6] * 16 + n[7]) << 24) | ((n[0] * 16 + n[1]) << 16) | ((n[2] * 16 + n[3]) << 8) |
            (n[4] * 16 + n[5]);
      return true;
    }
    return false;
  }
  if (starts_with(v, "light-dark(") && v.back() == ')') {
    std::string inner = v.substr(11, v.size() - 12);
    size_t comma = std::string::npos;
    int depth = 0;
    for (size_t k = 0; k < inner.size(); ++k) {
      if (inner[k] == '(') depth++;
      else if (inner[k] == ')') depth--;
      else if (inner[k] == ',' && depth == 0) { comma = k; break; }
    }
    if (comma != std::string::npos) {
      std::string c_light = inner.substr(0, comma);
      std::string c_dark = inner.substr(comma + 1);
      if (parse_colour(c_dark, out)) return true;
      if (parse_colour(c_light, out)) return true;
    }
    return false;
  }
  if (starts_with(v, "rgb")) {
    size_t open = v.find('(');
    if (open == std::string::npos) return false;
    float comp[4] = {0, 0, 0, 1};
    int count = 0;
    size_t i = open + 1;
    for (int c = 0; c < 4; ++c) {
      while (i < v.size() && (is_space(v[i]) || v[i] == ',' || v[i] == '/')) ++i;
      if (i >= v.size() || v[i] == ')') break;
      if (!parse_number(v, i, comp[c])) return false;
      ++count;
      bool pct = i < v.size() && v[i] == '%';
      if (pct) ++i;
      if (c < 3 && pct) comp[c] = comp[c] * 255.0f / 100.0f;
      if (c == 3 && pct) comp[c] = comp[c] / 100.0f;
    }
    if (count < 3) return false;
    auto clamp8 = [](float f) {
      int x = round_to_int(f);
      return (uint32_t)(x < 0 ? 0 : x > 255 ? 255 : x);
    };
    uint32_t a = clamp8(comp[3] * 255.0f);
    out = (a << 24) | (clamp8(comp[0]) << 16) | (clamp8(comp[1]) << 8) | clamp8(comp[2]);
    return true;
  }
  if (starts_with(v, "hsl")) {
    size_t open = v.find('(');
    if (open == std::string::npos) return false;
    float comp[4] = {0, 0, 0, 1};
    int count = 0;
    size_t i = open + 1;
    for (int c = 0; c < 4; ++c) {
      while (i < v.size() && (is_space(v[i]) || v[i] == ',' || v[i] == '/')) ++i;
      if (i >= v.size() || v[i] == ')') break;
      if (!parse_number(v, i, comp[c])) return false;
      ++count;
      bool pct = i < v.size() && v[i] == '%';
      if (pct) ++i;
      if (c == 0 && i + 3 <= v.size() && v.substr(i, 3) == "deg") i += 3;
      if (c > 0 && c < 3 && pct) comp[c] /= 100.0f;
      if (c == 3 && pct) comp[c] /= 100.0f;
    }
    if (count < 3) return false;
    float h = comp[0] / 360.0f;
    h = h - (float)(int)h;
    if (h < 0.0f) h += 1.0f;
    float s = std::max(0.0f, std::min(1.0f, comp[1]));
    float l = std::max(0.0f, std::min(1.0f, comp[2]));
    auto hue_to_rgb = [](float p, float q, float t) {
      if (t < 0.0f) t += 1.0f;
      if (t > 1.0f) t -= 1.0f;
      if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
      if (t < 1.0f / 2.0f) return q;
      if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
      return p;
    };
    float r = l, g = l, b = l;
    if (s > 0.0001f) {
      float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
      float p = 2.0f * l - q;
      r = hue_to_rgb(p, q, h + 1.0f / 3.0f);
      g = hue_to_rgb(p, q, h);
      b = hue_to_rgb(p, q, h - 1.0f / 3.0f);
    }
    auto clamp8 = [](float f) {
      int x = round_to_int(f * 255.0f);
      return (uint32_t)(x < 0 ? 0 : x > 255 ? 255 : x);
    };
    uint32_t a = (uint32_t)round_to_int(std::max(0.0f, std::min(1.0f, comp[3])) * 255.0f);
    out = (a << 24) | (clamp8(r) << 16) | (clamp8(g) << 8) | clamp8(b);
    return true;
  }
  if (v.back() == ')' && parse_colour_modern(v, out)) return true;
  for (const auto& c : kColours) {
    if (v == c.name) {
      out = c.argb;
      return true;
    }
  }
  return false;
}

// eval_calc is defined below, after that the complete `Length` type is in
// scope; parse_length calls it for `calc(...)` values.
Length eval_calc(const std::string& inner, int font_px, int root_font_px);

// A length in the units a page writes. `em` and `%` need context, so they come
// back as a Length for layout to resolve; everything absolute is resolved here.
Length parse_length(const std::string& raw, int font_px, int root_font_px) {
  Length len;
  std::string v = trimmed(lowered(raw));
  if (v.empty() || v == "auto") return len;
  if (v.compare(0, 5, "calc(") == 0 && v.back() == ')')
    return eval_calc(v.substr(5, v.size() - 6), font_px, root_font_px);
  size_t i = 0;
  float n = 0;
  if (!parse_number(v, i, n)) return len;
  std::string unit = trimmed(v.substr(i));
  if (unit.empty() || unit == "px") {
    len.unit = Length::Unit::Px;
    len.value = n;
  } else if (unit == "%") {
    len.unit = Length::Unit::Percent;
    len.value = n;
  } else if (unit == "em") {
    len.unit = Length::Unit::Px;
    len.value = n * (float)font_px;
  } else if (unit == "rem") {
    len.unit = Length::Unit::Px;
    len.value = n * (float)root_font_px;
  } else if (unit == "pt") {
    len.unit = Length::Unit::Px;
    len.value = n * 96.0f / 72.0f;
  } else if (unit == "pc") {
    len.unit = Length::Unit::Px;
    len.value = n * 16.0f;
  } else if (unit == "in") {
    len.unit = Length::Unit::Px;
    len.value = n * 96.0f;
  } else if (unit == "cm") {
    len.unit = Length::Unit::Px;
    len.value = n * 96.0f / 2.54f;
  } else if (unit == "mm") {
    len.unit = Length::Unit::Px;
    len.value = n * 96.0f / 25.4f;
  } else if (unit == "ex" || unit == "ch") {
    len.unit = Length::Unit::Px;
    len.value = n * (float)font_px * 0.5f;
  } else if (unit == "vw" || unit == "vmin" || unit == "vmax") {
    len.unit = Length::Unit::Vw;
    len.value = n;
  } else if (unit == "vh" || unit == "dvh" || unit == "svh" || unit == "lvh") {
    // The dynamic/small/large viewport units all mean the same thing on a pane
    // that does not have a retracting browser chrome.
    len.unit = Length::Unit::Vh;
    len.value = n;
  } else {
    return len;         // an unknown unit is not a length
  }
  return len;
}

// Evaluate a simple `calc()`. Operands parse like parse_length; like units
// combine (px-px and percent-percent); anything else falls back to Auto so
// the caller uses its normal default instead of a made-up number.
// `calc()`, as arithmetic rather than as one special case. Two precedence
// levels, each split at the LAST top-level operator so the parse is
// left-associative: `calc(100% - 2rem + 4px)` used to take the first `-` and
// hand the whole tail to one operand, and `calc(100%/3)` was not arithmetic at
// all -- it returned `auto`, which is how a three-column layout became one.
Length eval_calc_sum(const std::string& body, int font_px, int root_font_px);

// A bare number, for the right-hand side of `* 2` and `/ 3`.
bool calc_scalar(const std::string& t, float& out) {
  size_t i = 0;
  if (!parse_number(t, i, out)) return false;
  return trimmed(t.substr(i)).empty();
}

Length eval_calc_product(const std::string& raw, int font_px, int root_font_px) {
  const std::string body = trimmed(raw);
  if (body.empty()) return Length{};
  int depth = 0;
  size_t op = std::string::npos;
  char opc = 0;
  for (size_t k = 0; k < body.size(); ++k) {
    const char c = body[k];
    if (c == '(') { ++depth; continue; }
    if (c == ')') { --depth; continue; }
    if (depth == 0 && (c == '*' || c == '/')) { op = k; opc = c; }
  }
  if (op == std::string::npos) {
    // A parenthesised group, or a plain length.
    if (body.front() == '(' && body.back() == ')')
      return eval_calc_sum(body.substr(1, body.size() - 2), font_px, root_font_px);
    return parse_length(body, font_px, root_font_px);
  }
  if (op == 0 || op + 1 >= body.size()) return Length{};
  const std::string a = trimmed(body.substr(0, op));
  const std::string b = trimmed(body.substr(op + 1));
  // A length times a number, or divided by one. Length times length is not a
  // length, and the spec says so too.
  float scalar = 0;
  if (opc == '*' && calc_scalar(a, scalar)) {
    Length l = eval_calc_product(b, font_px, root_font_px);
    if (l.unit == Length::Unit::Auto) return Length{};
    l.value *= scalar;
    l.offset_px *= scalar;
    return l;
  }
  Length l = eval_calc_product(a, font_px, root_font_px);
  if (l.unit == Length::Unit::Auto || !calc_scalar(b, scalar)) return Length{};
  if (opc == '/') {
    if (scalar == 0) return Length{};
    l.value /= scalar;
    l.offset_px /= scalar;
  } else {
    l.value *= scalar;
    l.offset_px *= scalar;
  }
  return l;
}

Length eval_calc_sum(const std::string& raw, int font_px, int root_font_px) {
  const std::string body = trimmed(raw);
  if (body.empty()) return Length{};
  int depth = 0;
  size_t op = std::string::npos;
  char opc = 0;
  for (size_t k = 0; k < body.size(); ++k) {
    const char c = body[k];
    if (c == '(') { ++depth; continue; }
    if (c == ')') { --depth; continue; }
    if (depth != 0 || (c != '+' && c != '-')) continue;
    // In `calc()` a `+`/`-` operator MUST be surrounded by whitespace, which
    // is exactly what tells it apart from a sign (`-4px`) and from the `-` in
    // an identifier such as a custom property name.
    if (k == 0 || k + 1 >= body.size()) continue;
    if (!is_space(body[k - 1]) || !is_space(body[k + 1])) continue;
    op = k;
    opc = c;
  }
  if (op == std::string::npos) return eval_calc_product(body, font_px, root_font_px);
  const Length la = eval_calc_sum(body.substr(0, op), font_px, root_font_px);
  const Length lb = eval_calc_product(body.substr(op + 1), font_px, root_font_px);
  if (la.unit == Length::Unit::Auto || lb.unit == Length::Unit::Auto) return Length{};
  const float sign = opc == '+' ? 1.0f : -1.0f;
  if (la.unit == Length::Unit::Px && lb.unit == Length::Unit::Px) {
    Length out;
    out.unit = Length::Unit::Px;
    out.value = la.value + sign * lb.value;
    out.offset_px = la.offset_px + sign * lb.offset_px;
    return out;
  }
  // A mixed sum is one RELATIVE term plus a pixel term, which is the whole of
  // what a Length can carry: `calc(100% - 40px)` -- how "the full width less
  // the gutter" is always written -- and equally `calc(100vw - 20px)`, which
  // every full-bleed element and every `max-width` on a popover uses.
  //
  // `vw`/`vh` used to fall into the percentage branch, where the `: la.value`
  // arm silently counted `100vw` as ONE HUNDRED PIXELS. `calc(100vw - 20px)`
  // came out as 80px, so a dropdown's `max-width` squeezed it to the width of
  // two of its three icons and the third hung outside the panel.
  auto relative = [](const Length& l) {
    return l.unit == Length::Unit::Percent || l.unit == Length::Unit::Vw || l.unit == Length::Unit::Vh;
  };
  const bool ra = relative(la), rb = relative(lb);
  // Two different relative units cannot be added into one Length. Rather than
  // invent a number, say auto and let the caller use its own default.
  if (ra && rb && la.unit != lb.unit) return Length{};
  Length out;
  out.unit = ra ? la.unit : lb.unit;
  const float a_rel = ra ? la.value : 0.0f;
  const float b_rel = rb ? lb.value : 0.0f;
  const float a_px = ra ? la.offset_px : la.value + la.offset_px;
  const float b_px = rb ? lb.offset_px : lb.value + lb.offset_px;
  out.value = a_rel + sign * b_rel;
  out.offset_px = a_px + sign * b_px;
  return out;
}

Length eval_calc(const std::string& inner, int font_px, int root_font_px) {
  return eval_calc_sum(inner, font_px, root_font_px);
}

int px_or(const Length& l, int fallback) {
  if (l.unit == Length::Unit::Px) return round_to_int(l.value + l.offset_px);
  if (l.unit == Length::Unit::Vh) return round_to_int(l.value * 9.0f + l.offset_px);
  if (l.unit == Length::Unit::Vw) return round_to_int(l.value * 14.0f + l.offset_px);
  return fallback;
}

// Split a value on top-level whitespace, keeping `rgb(1, 2, 3)` and
// `url(a b c)` in one piece.
std::vector<std::string> split_parts(const std::string& v) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  for (char c : v) {
    if (c == '(') ++depth;
    if (c == ')' && depth > 0) --depth;
    if (is_space(c) && depth == 0) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
      continue;
    }
    cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// The address inside the first `url(...)` of a value, quotes stripped. A quoted
// address may hold ')' (an inline svg does), so it ends at its own quote.
bool css_url_arg(const std::string& v, std::string& out) {
  for (size_t i = 0; i + 4 <= v.size(); ++i) {
    if (lower(v[i]) != 'u' || lower(v[i + 1]) != 'r' || lower(v[i + 2]) != 'l' || v[i + 3] != '(') continue;
    size_t a = i + 4;
    while (a < v.size() && is_space(v[a])) ++a;
    if (a < v.size() && (v[a] == '"' || v[a] == '\'')) {
      const char q = v[a];
      size_t b = v.find(q, a + 1);
      if (b == std::string::npos) return false;
      out = v.substr(a + 1, b - a - 1);
      return true;
    }
    size_t b = v.find(')', a);
    if (b == std::string::npos) return false;
    while (b > a && is_space(v[b - 1])) --b;
    out = v.substr(a, b - a);
    return true;
  }
  return false;
}

// The first comma-separated layer of a `background` value: with several
// pictures the topmost is the first, and it is the one painted here.
std::string css_first_layer(const std::string& v) {
  int depth = 0;
  char quote = 0;
  for (size_t i = 0; i < v.size(); ++i) {
    const char c = v[i];
    if (quote) { if (c == quote) quote = 0; continue; }
    if (c == '"' || c == '\'') quote = c;
    else if (c == '(') ++depth;
    else if (c == ')' && depth > 0) --depth;
    else if (c == ',' && depth == 0) return v.substr(0, i);
  }
  return v;
}

// One `background-position` / `object-position` component as a percent of the
// free space plus a px offset: `left`, `50%`, `12px`.
bool css_pos_token(const std::string& t, int em, int root_px, float& pct, float& off, bool& is_x_kw, bool& is_y_kw) {
  is_x_kw = is_y_kw = false;
  pct = 0; off = 0;
  if (t == "left") { is_x_kw = true; return true; }
  if (t == "right") { pct = 100; is_x_kw = true; return true; }
  if (t == "top") { is_y_kw = true; return true; }
  if (t == "bottom") { pct = 100; is_y_kw = true; return true; }
  if (t == "center") { pct = 50; return true; }
  Length l = parse_length(t, em, root_px);
  if (l.unit == Length::Unit::Percent) { pct = l.value; return true; }
  if (l.unit == Length::Unit::Px) { off = l.value + l.offset_px; return true; }
  return false;
}

// One or two position tokens, in either keyword order (`top left` = `left top`).
void css_position(const std::vector<std::string>& tok, int em, int root_px, float& px, float& py, float& ox, float& oy) {
  if (tok.empty() || tok.size() > 2) return;
  float p0 = 50, o0 = 0, p1 = 50, o1 = 0;
  bool x0 = false, y0 = false, x1 = false, y1 = false;
  if (!css_pos_token(tok[0], em, root_px, p0, o0, x0, y0)) return;
  const bool two = tok.size() == 2;
  if (two && !css_pos_token(tok[1], em, root_px, p1, o1, x1, y1)) return;
  if (!two) {
    if (y0) { py = p0; oy = o0; px = 50; ox = 0; }
    else { px = p0; ox = o0; py = 50; oy = 0; }
    return;
  }
  if (y0 || x1) { px = p1; ox = o1; py = p0; oy = o0; }
  else { px = p0; ox = o0; py = p1; oy = o1; }
}

// `background-repeat`: one or two keywords, across then down.
uint8_t css_repeat(const std::vector<std::string>& tok, uint8_t dflt) {
  if (tok.empty()) return dflt;
  auto on = [](const std::string& t) { return t != "no-repeat"; };
  if (tok[0] == "repeat-x") return 1;
  if (tok[0] == "repeat-y") return 2;
  if (tok[0] != "repeat" && tok[0] != "no-repeat" && tok[0] != "space" && tok[0] != "round") return dflt;
  const bool x = on(tok[0]), y = tok.size() > 1 ? on(tok[1]) : x;
  return (uint8_t)((x ? 1 : 0) | (y ? 2 : 0));
}

// `background-size`: `cover`, `contain`, or one or two lengths.
void css_bg_size(Style& s, const std::vector<std::string>& tok, int em, int root_px) {
  if (tok.empty()) return;
  if (tok[0] == "cover") { s.bg_size = Style::BgSize::Cover; return; }
  if (tok[0] == "contain") { s.bg_size = Style::BgSize::Contain; return; }
  if (tok[0] == "auto" && tok.size() == 1) { s.bg_size = Style::BgSize::Auto; return; }
  s.bg_size = Style::BgSize::Explicit;
  s.bg_size_w = parse_length(tok[0], em, root_px);
  s.bg_size_h = tok.size() > 1 ? parse_length(tok[1], em, root_px) : Length();
}

std::vector<std::string> css_lower_parts(const std::string& v) {
  std::vector<std::string> t = split_parts(css_first_layer(v));
  for (auto& x : t) for (auto& ch : x) ch = lower(ch);
  return t;
}

// The `background` shorthand's picture, repeat, size and position; the colour
// is read separately. Only the first layer is painted.
void css_background_shorthand(Style& s, const std::string& value, int em, int root_px) {
  const std::string layer = css_first_layer(value);
  s.bg_image.clear();
  s.bg_repeat = 3;
  s.bg_size = Style::BgSize::Auto;
  s.bg_pos_x = s.bg_pos_y = s.bg_off_x = s.bg_off_y = 0;
  std::string url;
  if (css_url_arg(layer, url)) s.bg_image = url;
  // `center / cover`: the slash separates position from size, but only outside url(...).
  std::string spaced;
  int depth = 0;
  char quote = 0;
  for (char c : layer) {
    if (quote) { if (c == quote) quote = 0; }
    else if (c == '"' || c == '\'') quote = c;
    else if (c == '(') ++depth;
    else if (c == ')' && depth > 0) --depth;
    if (c == '/' && depth == 0 && !quote) { spaced += " / "; continue; }
    spaced += c;
  }
  std::vector<std::string> pos, size, rep;
  bool in_size = false;
  for (auto part : split_parts(spaced)) {
    for (auto& ch : part) ch = lower(ch);
    if (part == "/") { in_size = true; continue; }
    if (part.compare(0, 4, "url(") == 0 || part.find("gradient(") != std::string::npos) continue;
    if (part == "repeat" || part == "no-repeat" || part == "repeat-x" || part == "repeat-y" || part == "space" ||
        part == "round") { rep.push_back(part); continue; }
    if (part == "scroll" || part == "fixed" || part == "local" || part == "border-box" || part == "padding-box" ||
        part == "content-box" || part == "none")
      continue;
    uint32_t cc = 0;
    if (!in_size && parse_colour(part, cc)) continue;
    (in_size ? size : pos).push_back(part);
  }
  s.bg_repeat = css_repeat(rep, 3);
  css_bg_size(s, size, em, root_px);
  css_position(pos, em, root_px, s.bg_pos_x, s.bg_pos_y, s.bg_off_x, s.bg_off_y);
}

// The one-to-four value shorthand: `10px`, `10px 20px`, `10px 20px 30px`,
// `10px 20px 30px 40px`, in top/right/bottom/left order.
// The keyword `auto`, and nothing else -- parse_length hands back an Auto length
// for anything it cannot read, and `inherit` is not a margin to be shared out.
bool is_auto_keyword(const std::string& v) {
  size_t a = 0, b = v.size();
  while (a < b && is_space(v[a])) ++a;
  while (b > a && is_space(v[b - 1])) --b;
  if (b - a != 4) return false;
  const char* w = "auto";
  for (size_t i = 0; i < 4; ++i) if (lower(v[a + i]) != w[i]) return false;
  return true;
}

void apply_box_shorthand(const std::string& value, int font_px, int root_px, int out[4], bool autos[4] = nullptr) {
  auto parts = split_parts(value);
  int v[4] = {0, 0, 0, 0};
  bool a[4] = {false, false, false, false};
  size_t n = parts.size() > 4 ? 4 : parts.size();
  for (size_t i = 0; i < n; ++i) {
    Length l = parse_length(parts[i], font_px, root_px);
    a[i] = autos && is_auto_keyword(parts[i]);
    v[i] = px_or(l, 0);
  }
  if (n == 0) return;
  int idx[4];   // which written value each side takes
  if (n == 1) { idx[0] = idx[1] = idx[2] = idx[3] = 0; }
  else if (n == 2) { idx[0] = idx[2] = 0; idx[1] = idx[3] = 1; }
  else if (n == 3) { idx[0] = 0; idx[1] = idx[3] = 1; idx[2] = 2; }
  else { idx[0] = 0; idx[1] = 1; idx[2] = 2; idx[3] = 3; }
  for (int i = 0; i < 4; ++i) {
    out[i] = v[idx[i]];
    if (autos) autos[i] = a[idx[i]];
  }
}

// ---------------------------------------------------------------- selectors --

bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

SimpleSelector parse_simple(const std::string& text) {
  SimpleSelector s;
  std::string cur;
  bool saw_pseudo = false;
  char mode = 0;   // 0 tag, '.' class, '#' id
  auto flush = [&]() {
    if (cur.empty()) return;
    if (mode == '.') s.classes.push_back(cur);
    else if (mode == '#') s.id = cur;
    else s.tag = cur;
    cur.clear();
  };
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '*') { s.universal = true; continue; }
    if (c == '.' || c == '#') {
      flush();
      mode = c;
      continue;
    }
    if (c == '[') {
      flush();
      mode = 0;
      size_t close = text.find(']', i);
      std::string body = text.substr(i + 1, (close == std::string::npos ? text.size() : close) - i - 1);
      // `[href]` is presence; `[type="submit"]`, `[href^="https"]` and the
      // rest compare the value. These all used to collapse to presence, so a
      // rule written for one control matched every element with the attribute.
      SimpleSelector::AttrTest t;
      size_t eq = body.find('=');
      if (eq == std::string::npos) {
        t.name = lowered(trimmed(body));
        t.op = SimpleSelector::AttrTest::Op::Present;
      } else {
        std::string name = trimmed(body.substr(0, eq));
        char prefix = name.empty() ? '\0' : name.back();
        switch (prefix) {
          case '~': t.op = SimpleSelector::AttrTest::Op::Includes; name.pop_back(); break;
          case '|': t.op = SimpleSelector::AttrTest::Op::DashMatch; name.pop_back(); break;
          case '^': t.op = SimpleSelector::AttrTest::Op::Prefix; name.pop_back(); break;
          case '$': t.op = SimpleSelector::AttrTest::Op::Suffix; name.pop_back(); break;
          case '*': t.op = SimpleSelector::AttrTest::Op::Contains; name.pop_back(); break;
          default: t.op = SimpleSelector::AttrTest::Op::Equals; break;
        }
        t.name = lowered(trimmed(name));
        std::string v = trimmed(body.substr(eq + 1));
        // The value may be quoted with either quote, and an `i` flag may follow
        // it. Matching is case-sensitive on the value either way -- the flag is
        // consumed so it does not end up inside the string being compared.
        if (v.size() >= 2 && (v.back() == 'i' || v.back() == 'I') &&
            (v[v.size() - 2] == ' ' || v[v.size() - 2] == '"' || v[v.size() - 2] == '\'')) {
          v = trimmed(v.substr(0, v.size() - 1));
        }
        if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\'')))
          v = v.substr(1, v.size() - 2);
        t.value = v;
        // `[x=]` with nothing to compare is not a selector anyone meant; treat
        // it as presence rather than as "equals the empty string", which would
        // match nothing and silently drop the rule.
        if (t.value.empty()) t.op = SimpleSelector::AttrTest::Op::Present;
      }
      s.attrs.push_back(std::move(t));
      i = close == std::string::npos ? text.size() : close;
      continue;
    }
    if (c == ':') {
      flush();
      mode = 0;
      saw_pseudo = true;
      // `::x` is a pseudo-ELEMENT: it styles generated content that no element
      // in this document has, so nothing here can match it.
      const bool double_colon = (i + 1 < text.size() && text[i + 1] == ':');
      size_t name_start = i + (double_colon ? 2 : 1);
      size_t name_end = name_start;
      while (name_end < text.size() &&
             (is_alpha(text[name_end]) || text[name_end] == '-' ||
              (text[name_end] >= '0' && text[name_end] <= '9'))) ++name_end;
      const std::string name = lowered(text.substr(name_start, name_end - name_start));
      if (double_colon || name == "before" || name == "after" || name == "first-line" ||
          name == "first-letter" || name == "selection" || name == "placeholder" ||
          name == "marker" || name == "backdrop") {
        s.never = true;
        // ...but remember WHICH, for the two that carry content a page
        // actually draws. The compound still matches nothing in the ordinary
        // cascade; pseudo_content() below is what runs these rules.
        if (name == "before") s.pseudo = SimpleSelector::PseudoEl::Before;
        else if (name == "after") s.pseudo = SimpleSelector::PseudoEl::After;
      }
      // Structural pseudo-classes this engine CAN evaluate against a node's
      // siblings, so a rule like `li:first-child` stops smearing across every
      // sibling. Recorded here, tested in matches_simple.
      if (name == "first-child") { s.first_child = true; saw_pseudo = false; }
      else if (name == "last-child") { s.last_child = true; saw_pseudo = false; }
      else if (name == "only-child") { s.only_child = true; saw_pseudo = false; }
      else if (name == "root") { s.is_root = true; saw_pseudo = false; }
      else if (name == "nth-child") {
        // arg is captured below (this branch is inside the functional-pseudo
        // block only when there is a parenthesis); handle there.
        s.has_nth = true;
      }
      // A DYNAMIC state this page is not in. `:hover` used to be dropped from
      // the compound, which applies the hovered look permanently -- and a page
      // keeps its hover-revealed controls at `opacity: 0` and turns them on
      // with `:hover`, so dropping it put an edit button on top of every
      // message bubble and a menu on every row. What is drawn here is the
      // page AT REST, and none of these hold at rest.
      if (name == "hover") {
        // Decidable now: the pane marks the pointer's element and its ancestors with
        // this attribute and re-cascades when the pointer moves to another element.
        SimpleSelector::AttrTest t;
        t.name = "data-aios-hover";
        t.op = SimpleSelector::AttrTest::Op::Present;
        s.attrs.push_back(std::move(t));
        saw_pseudo = false;
        g_css_has_hover = true;
      } else if (name == "active" || name == "focus" || name == "focus-visible" ||
          name == "focus-within" || name == "target" || name == "visited" || name == "current" ||
          name == "past" || name == "future" || name == "playing" || name == "paused" ||
          name == "user-invalid" || name == "user-valid") {
        s.never = true;
        s.never_unevaluable = true;
      }
      // States that ARE decidable, because they are attributes: `:checked` is
      // the `checked` attribute and `:disabled` is the `disabled` one. These
      // were dropped too, so a checked box and an unchecked one styled the
      // same.
      else if (name == "checked" || name == "disabled" || name == "required" ||
               name == "readonly" || name == "open" || name == "selected" ||
               name == "multiple" || name == "autofocus") {
        SimpleSelector::AttrTest t;
        t.name = name == "readonly" ? "readonly" : name;
        t.op = SimpleSelector::AttrTest::Op::Present;
        s.attrs.push_back(std::move(t));
        saw_pseudo = false;
      } else if (name == "enabled") {
        s.not_lists.push_back("[disabled]");
        saw_pseudo = false;
      } else if (name == "link" || name == "any-link") {
        // The resting state of a link, which is what is being drawn.
        saw_pseudo = false;
      }
      // Everything else (`:first-of-type` and friends) keeps the old
      // behaviour of being dropped: over-applying a structural rule is milder
      // than dropping the page's styling for it.
      // A functional pseudo takes a parenthesised argument, which has to be
      // skipped as a unit — walking forward to the next '.' or '#' lands
      // INSIDE it and turns `:not(.is-expanded)` into the class `is-expanded)`.
      if (name_end < text.size() && text[name_end] == '(') {
        size_t depth = 0, j = name_end;
        for (; j < text.size(); ++j) {
          if (text[j] == '(') ++depth;
          else if (text[j] == ')') { if (--depth == 0) break; }
        }
        const std::string arg = trimmed(text.substr(name_end + 1, j - name_end - 1));
        if (name == "not" && !arg.empty()) {
          // Flattened: `:not(.x)`, `:not(#y)`, `:not(tag)` is the whole of
          // real-world usage, and evaluating it is what keeps a responsive
          // rule set from being half-applied.
          const std::string a = lowered(arg);
          const bool simple = a.find(',') == std::string::npos &&
                              a.find(' ') == std::string::npos &&
                              a.find('[') == std::string::npos &&
                              a.find(':') == std::string::npos;
          if (simple && a[0] == '.') s.not_classes.push_back(a.substr(1));
          else if (simple && a[0] == '#') s.not_ids.push_back(a.substr(1));
          else if (simple && is_alpha(a[0]) && a.find('.') == std::string::npos) s.not_tags.push_back(a);
          else s.not_lists.push_back(arg);   // evaluated by re-entering the matcher
          saw_pseudo = false;   // this compound has something concrete to match on
        } else if (name == "nth-child") {
          s.nth_child = arg;
          s.has_nth = true;
        } else if (name == "is" || name == "where") {
          // These used to be accepted and their argument thrown away, which
          // made `:is(h1, h2)` match every element on the page.
          if (!arg.empty()) { s.is_lists.push_back(arg); saw_pseudo = false; }
          if (name == "where") s.where_only = true;
        } else if (true) {
          // :has and friends: this engine cannot tell whether they
          // hold, and guessing "yes" is what over-applied every rule.
          s.never = true;
          s.never_unevaluable = true;
        }
        i = (j < text.size()) ? j : text.size();
        continue;
      }
      i = name_end > i ? name_end - 1 : i;
      continue;
    }
    cur += lower(c);
  }
  flush();
  // Nothing concrete left. Universal ONLY when the selector really was `*` or
  // empty — never as the fallback for a compound whose only content was a
  // pseudo, which is how `::-webkit-scrollbar { width: 10px }` came to set the
  // width of every element on the page.
  if (s.tag.empty() && s.id.empty() && s.classes.empty() && s.attrs.empty() &&
      s.is_lists.empty() && s.not_lists.empty()) {
    if (saw_pseudo && !s.universal) s.never = true;
    else s.universal = true;
  }
  return s;
}

Selector parse_selector(const std::string& text) {
  Selector sel;
  std::string cur;
  // The combinator that precedes the NEXT part. `a b` = descendant,
  // `a > b` = child, `a + b` = adjacent sibling, `a ~ b` = general sibling.
  Combinator pending = Combinator::Descendant;
  auto flush = [&]() {
    if (trimmed(cur).empty()) return;
    SimpleSelector p = parse_simple(trimmed(cur));
    p.prev = pending;
    sel.parts.push_back(std::move(p));
    cur.clear();
  };
  // Depth and quote state, because a combinator is only a combinator at the top
  // level. `[class~="tag"]` split at the tilde into two nonsense compounds and
  // matched nothing; `[class="tag important"]` split at the space; and
  // `:nth-child(2n + 1)` split at the plus. All three were silently dropped
  // rules, which is the failure mode that looks like the cascade is fine.
  int brackets = 0, parens = 0;
  char quote = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (quote) {
      cur += c;
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '"' || c == '\'') { quote = c; cur += c; continue; }
    if (c == '[') ++brackets;
    else if (c == ']') { if (brackets) --brackets; }
    else if (c == '(') ++parens;
    else if (c == ')') { if (parens) --parens; }
    if (brackets || parens) { cur += c; continue; }
    if (is_space(c)) {
      flush();
      // Whitespace sits between two parts (descendant) OR around a single
      // `>/+ /~`. Peek past the space for a combinator token.
      size_t j = i;
      while (j < text.size() && is_space(text[j])) ++j;
      if (j < text.size() && (text[j] == '>' || text[j] == '+' || text[j] == '~')) {
        pending = text[j] == '>' ? Combinator::Child
                : text[j] == '+' ? Combinator::Adjacent
                                 : Combinator::Sibling;
        // Consume the combinator and any whitespace after it.
        i = j + 1;
        while (i < text.size() && is_space(text[i])) ++i;
        if (i > 0) --i;   // outer loop ++i lands exactly on the next token
        cur.clear();
        continue;
      }
      continue;   // plain space between parts: descendant; next token not consumed
    }
    if (c == '>' || c == '+' || c == '~') {
      flush();
      pending = c == '>' ? Combinator::Child
              : c == '+' ? Combinator::Adjacent
                         : Combinator::Sibling;
      // Skip whitespace after `> / + / ~`.
      while (i + 1 < text.size() && is_space(text[i + 1])) ++i;
      cur.clear();
      continue;
    }
    cur += c;
  }
  flush();
  return sel;
}

bool match_structural(const Node& p, const Node& el, const SimpleSelector& s);
bool nth_matches(const std::string& expr, int pos);

// The element's id equals `want` (already lower case), ignoring case, without
// making a lowered copy of the attribute for every selector tested.
bool id_is(const Node& el, const std::string& want) {
  const std::string& id = el.attr_ref("id");
  if (id.size() != want.size()) return false;
  for (size_t k = 0; k < id.size(); ++k)
    if (lower(id[k]) != want[k]) return false;
  return true;
}

bool has_class(const Node& el, const std::string& want) {
  // Compared in place, case-insensitively against the (already lowered) selector
  // name: no copy of the attribute, no copy of each token. This ran for every
  // class of every selector on every element, and was a third of a page load.
  const std::string& cls = el.attr_ref("class");
  const size_t n = cls.size(), wn = want.size();
  if (n < wn) return false;
  if (n == wn) {
    for (size_t k = 0; k < wn; ++k)
      if (lower(cls[k]) != want[k]) return false;
    return true;
  }
  size_t i = 0;
  while (i < n) {
    while (i < n && is_space(cls[i])) ++i;
    const size_t start = i;
    while (i < n && !is_space(cls[i])) ++i;
    if (i - start == wn) {
      size_t k = 0;
      while (k < wn && lower(cls[start + k]) == want[k]) ++k;
      if (k == wn) return true;
    }
  }
  return false;
}

bool matches_simple(const Node& el, const SimpleSelector& s) {
  if (el.kind != Node::Kind::Element) return false;
  if (s.never) return false;
  if (!s.tag.empty() && el.tag != s.tag) return false;
  if (!s.id.empty()) {
    const std::string& id = el.attr_ref("id");
    if (id.empty() || !id_is(el, s.id)) return false;
  }
  if (!s.classes.empty()) {
    const std::string& cls = el.attr_ref("class");
    if (cls.empty()) return false;
    for (const auto& c : s.classes)
      if (!has_class(el, c)) return false;
  }
  for (const auto& a : s.attrs)
    if (!a.matches(el)) return false;
  for (const auto& t : s.not_tags)
    if (el.tag == t) return false;
  for (const auto& i : s.not_ids)
    if (id_is(el, i)) return false;
  for (const auto& c : s.not_classes)
    if (has_class(el, c)) return false;
  for (const auto& list : s.is_lists)
    if (!selector_matches_text(el, list)) return false;
  for (const auto& list : s.not_lists)
    if (selector_matches_text(el, list)) return false;
  if (!el.parent) return s.is_root;
  return match_structural(*el.parent, el, s);
}

// Structural pseudo-classes: where the element sits among its siblings. These
// replace the old "drop it and apply to every sibling" behaviour, which is how
// one `li:first-child { margin-top: 0 }` nudged every row of a list.
bool match_structural(const Node& p, const Node& el, const SimpleSelector& s) {
  if (s.first_child || s.last_child || s.only_child || s.has_nth || s.is_root) {
    if (s.is_root && p.tag != "html") return false;
    if (s.first_child && !s.last_child && !s.only_child && !s.has_nth) {
      for (const NodePtr& ch : p.children) {
        if (ch->kind != Node::Kind::Element) continue;
        return ch.get() == &el;
      }
      return false;
    }
    int idx = 0, count = 0;
    for (const NodePtr& ch : p.children) {
      if (ch->kind != Node::Kind::Element) continue;
      if (ch.get() == &el) idx = count;   // 1-based position of this element
      ++count;
    }
    const int pos = idx + 1;
    if (s.first_child && pos != 1) return false;
    if (s.last_child && pos != count) return false;
    if (s.only_child && count != 1) return false;
    if (s.has_nth && !nth_matches(s.nth_child, pos)) return false;
  }
  return true;
}

// `odd`, `even`, `<an+b>` (also written `2n`, `3n+1`, `-n+5`…) as given.
// `an+b` against a 1-based position. An element matches when there is a
// non-negative integer k with pos == a*k + b -- which is the definition, and
// not what this used to compute. It evaluated `a*pos - a + b` and asked whether
// that was a non-negative multiple of a, so `2n+1` (the odd children) matched
// nothing at all, and `2n + 1` -- the same selector written with spaces, which
// strtol stops on -- read b as 0 and matched EVERY child.
//
// All whitespace inside the expression goes first, so `2n + 1`, `2n+1` and
// ` 2N + 1 ` are one selector rather than three behaviours.
bool nth_matches(const std::string& expr, int pos) {
  std::string e;
  for (char c : expr) if (!is_space(c)) e += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  if (e == "odd") return (pos & 1) == 1;
  if (e == "even") return (pos & 1) == 0;
  if (e.empty()) return false;

  size_t n = e.find('n');
  if (n == std::string::npos) {
    // A bare number: only that one child.
    return (long)pos == strtol(e.c_str(), nullptr, 10);
  }
  const std::string a_str = e.substr(0, n);
  const std::string b_str = e.substr(n + 1);
  long a = 1;
  if (a_str.empty() || a_str == "+") a = 1;
  else if (a_str == "-") a = -1;
  else a = strtol(a_str.c_str(), nullptr, 10);
  long b = 0;
  if (!b_str.empty()) {
    // `n+3`, `n-3`; a lone sign with no digits is not a number.
    const char* p = b_str.c_str();
    if ((b_str[0] == '+' || b_str[0] == '-') && b_str.size() == 1) b = 0;
    else b = strtol(p, nullptr, 10);
  }

  if (a == 0) return (long)pos == b;
  const long diff = (long)pos - b;
  if (diff % a != 0) return false;
  return diff / a >= 0;
}

// The element immediately before `el` among its parent's element children, or
// nullptr if `el` is its parent's first element child.
const Node* prev_element_sibling(const Node* el) {
  if (!el || !el->parent) return nullptr;
  const auto& kids = el->parent->children;
  size_t idx = kids.size();
  for (size_t i = 0; i < kids.size(); ++i) {
    if (kids[i].get() == el) { idx = i; break; }
  }
  for (size_t i = idx; i-- > 0;) {
    if (kids[i]->kind == Node::Kind::Element) return kids[i].get();
  }
  return nullptr;
}

// Match `el` against `sel`, walking the compound RIGHT-to-LEFT and applying each
// part's combinator (descendant / child / adjacent-sibling / general-sibling)
// to bound the next part, instead of the old "everything is any ancestor":
// that is what let `body > .toolbar` paint every `.toolbar` on the page.
// Ancestor filter. A descendant/child part of a selector can only match if
// some ancestor carries its tag, id and classes; a 256-bit bloom of every
// ancestor's features says "certainly not" for most of the failing selectors
// without the walk up the tree (Wikipedia: ~25 deep, hundreds of such rules).
struct AncBloom { uint64_t w[4] = {0, 0, 0, 0}; };
inline unsigned bloom_hash(const char* s, size_t n, unsigned seed) {
  unsigned h = 2166136261u ^ seed;
  for (size_t k = 0; k < n; ++k) { h ^= (unsigned char)lower(s[k]); h *= 16777619u; }
  return h ^ (h >> 15);
}
inline void bloom_add(AncBloom& b, unsigned h) { b.w[(h >> 6) & 3] |= 1ull << (h & 63); }
inline bool bloom_has(const AncBloom& b, unsigned h) { return (b.w[(h >> 6) & 3] >> (h & 63)) & 1; }
void bloom_add_node(AncBloom& b, const Node& n) {
  if (n.kind != Node::Kind::Element) return;
  bloom_add(b, bloom_hash(n.tag.data(), n.tag.size(), 1));
  const std::string& id = n.attr_ref("id");
  if (!id.empty()) bloom_add(b, bloom_hash(id.data(), id.size(), 2));
  const std::string& cls = n.attr_ref("class");
  size_t i = 0;
  while (i < cls.size()) {
    while (i < cls.size() && is_space(cls[i])) ++i;
    const size_t st = i;
    while (i < cls.size() && !is_space(cls[i])) ++i;
    if (i > st) bloom_add(b, bloom_hash(cls.data() + st, i - st, 3));
  }
}
// `bloom` is set by compute_style for the element being matched (only then).
const Node* g_bloom_el = nullptr;
const AncBloom* g_bloom = nullptr;
// Whether the filter rules out `want` having a match among the ancestors.
bool bloom_rules_out(const AncBloom& b, const SimpleSelector& want) {
  if (want.never) return true;
  if (!want.tag.empty() && !bloom_has(b, bloom_hash(want.tag.data(), want.tag.size(), 1))) return true;
  if (!want.id.empty() && !bloom_has(b, bloom_hash(want.id.data(), want.id.size(), 2))) return true;
  for (const auto& c : want.classes)
    if (!bloom_has(b, bloom_hash(c.data(), c.size(), 3))) return true;
  return false;
}

bool matches(const Node& el, const Selector& sel) {
  if (sel.parts.empty()) return false;
  if (!matches_simple(el, sel.parts.back())) return false;
  if (sel.parts.size() == 1) return true;
  size_t i = sel.parts.size() - 1;
  if (g_bloom && g_bloom_el == &el) {
    // Parts reached through descendant/child steps alone are ancestors of el.
    for (size_t k = i; k > 0; --k) {
      const Combinator c = sel.parts[k].prev;
      if (c != Combinator::Descendant && c != Combinator::Child) break;
      if (bloom_rules_out(*g_bloom, sel.parts[k - 1])) return false;
    }
  }
  const Node* cur = &el;
  while (i > 0) {
    const Combinator comb = sel.parts[i].prev;
    const SimpleSelector& want = sel.parts[i - 1];
    const Node* hit = nullptr;
    if (comb == Combinator::Descendant) {
      for (const Node* p = cur->parent; p; p = p->parent) {
        if (matches_simple(*p, want)) { hit = p; break; }
      }
    } else if (comb == Combinator::Child) {
      if (cur->parent && matches_simple(*cur->parent, want)) hit = cur->parent;
    } else if (comb == Combinator::Adjacent) {
      const Node* sib = prev_element_sibling(cur);
      if (sib && matches_simple(*sib, want)) hit = sib;
    } else {  // Combinator::Sibling  (~)
      if (!cur->parent) return false;
      const auto& kids = cur->parent->children;
      size_t idx = kids.size();
      for (size_t k = 0; k < kids.size(); ++k) {
        if (kids[k].get() == cur) { idx = k; break; }
      }
      for (size_t k = idx; k-- > 0;) {
        if (kids[k]->kind == Node::Kind::Element && matches_simple(*kids[k], want)) {
          hit = kids[k].get();
          break;
        }
      }
    }
    if (!hit) return false;
    cur = hit;
    --i;
  }
  return true;
}

}  // namespace

bool css_colour(const std::string& value, uint32_t& out) { return parse_colour(value, out); }

// One attribute term against an element. The value comparisons are the CSS
// ones: `~=` is a word in a space-separated list (how `class` is written),
// `|=` is the value or the value followed by a hyphen (how `lang` is written).
bool SimpleSelector::AttrTest::matches(const Node& el) const {
  if (!el.has_attr(name)) return false;
  if (op == Op::Present) return true;
  const std::string got = el.attr(name);
  switch (op) {
    case Op::Equals: return got == value;
    case Op::Prefix: return got.size() >= value.size() && got.compare(0, value.size(), value) == 0;
    case Op::Suffix: return got.size() >= value.size() &&
                            got.compare(got.size() - value.size(), value.size(), value) == 0;
    case Op::Contains: return got.find(value) != std::string::npos;
    case Op::DashMatch: return got == value ||
                               (got.size() > value.size() && got.compare(0, value.size(), value) == 0 &&
                                got[value.size()] == '-');
    case Op::Includes: {
      // A word, whole: `[class~="x"]` must not match "xy" or "wx".
      size_t at = 0;
      while (at < got.size()) {
        while (at < got.size() && is_space(got[at])) ++at;
        size_t end = at;
        while (end < got.size() && !is_space(got[end])) ++end;
        if (end > at && got.compare(at, end - at, value) == 0) return true;
        at = end;
      }
      return false;
    }
    case Op::Present: return true;
  }
  return false;
}

int SimpleSelector::specificity() const {
  int spec = 0;
  if (!id.empty()) spec += 100;
  spec += (int)(classes.size() + attrs.size()) * 10;
  // `:is()` carries the weight of a class here rather than of its heaviest
  // argument, which is close enough to be right about the cascade in practice;
  // `:where()` is defined to carry none.
  if (!where_only) spec += (int)(is_lists.size() + not_lists.size()) * 10;
  if (!tag.empty()) spec += 1;
  // A negation contributes the specificity of its ARGUMENT, which is what
  // makes `#sidebar:not(.is-expanded)` beat a plain `#sidebar` — and getting
  // that wrong is a cascade that resolves the wrong way round.
  spec += (int)not_ids.size() * 100;
  spec += (int)not_classes.size() * 10;
  spec += (int)not_tags.size();
  return spec;
}

int Selector::specificity() const {
  int spec = 0;
  for (const auto& p : parts) spec += p.specificity();
  return spec;
}

void Stylesheet::append(const Stylesheet& other) {
  rules.insert(rules.end(), other.rules.begin(), other.rules.end());
  rule_index.reset();
  rule_index_n = 0;
}

int Length::resolve(int against, int fallback) const {
  if (unit == Unit::Px) return round_to_int(value + offset_px);
  if (unit == Unit::Percent) return round_to_int(value * (float)against / 100.0f + offset_px);
  return fallback;
}

// One selector list, matched. Parsed each call: a page's queries are short and
// this is not the cascade's hot path (that one keeps its rules parsed).
// Split a selector LIST on its top-level commas. A comma inside `:is(h1, h2)`
// or `[title="a,b"]` belongs to that term, not to the list -- splitting on
// every comma turned `:is(h1, h2)` into the two nonsense selectors `:is(h1`
// and `h2)`, so only the first alternative in any list was ever honoured.
std::vector<std::string> split_selector_list(const std::string& text) {
  std::vector<std::string> out;
  int parens = 0, brackets = 0;
  char quote = 0;
  size_t start = 0;
  for (size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size()) {
      std::string one = trimmed(text.substr(start));
      if (!one.empty()) out.push_back(std::move(one));
      break;
    }
    const char c = text[i];
    if (quote) { if (c == quote) quote = 0; continue; }
    if (c == '"' || c == '\'') { quote = c; continue; }
    if (c == '(') ++parens;
    else if (c == ')') { if (parens) --parens; }
    else if (c == '[') ++brackets;
    else if (c == ']') { if (brackets) --brackets; }
    else if (c == ',' && !parens && !brackets) {
      std::string one = trimmed(text.substr(start, i - start));
      if (!one.empty()) out.push_back(std::move(one));
      start = i + 1;
    }
  }
  return out;
}

static std::unordered_map<std::string, std::vector<Selector>> g_parsed_selector_cache;

bool selector_matches_text(const Node& el, const std::string& selector) {
  auto it = g_parsed_selector_cache.find(selector);
  if (it == g_parsed_selector_cache.end()) {
    std::vector<Selector> list;
    for (const std::string& one : split_selector_list(selector)) {
      Selector sel = parse_selector(one);
      if (!sel.parts.empty()) list.push_back(std::move(sel));
    }
    it = g_parsed_selector_cache.emplace(selector, std::move(list)).first;
  }
  for (const auto& sel : it->second) {
    if (matches(el, sel)) return true;
  }
  return false;
}

namespace {

// The text structure a browser insists on before it will run a selector at all.
bool selector_text_well_formed(const std::string& text) {
  std::string t = trimmed(text);
  if (t.empty()) return false;
  int parens = 0, brackets = 0;
  char quote = 0;
  bool prev_comma = true, prev_comb = true;   // at the start, a comma or a combinator would be dangling
  for (size_t i = 0; i < t.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(t[i]);
    if (quote) {
      if (c == '\\') ++i;
      else if (c == static_cast<unsigned char>(quote)) quote = 0;
      continue;
    }
    if (c == '"' || c == '\'') { quote = static_cast<char>(c); prev_comma = prev_comb = false; continue; }
    if (c == '\\') { ++i; prev_comma = prev_comb = false; continue; }
    if (c == '[') { ++brackets; prev_comma = prev_comb = false; continue; }
    if (c == ']') { if (--brackets < 0) return false; continue; }
    if (brackets) continue;   // an attribute selector's insides are its own grammar
    if (c == '(') { ++parens; prev_comma = prev_comb = false; continue; }
    if (c == ')') { if (--parens < 0) return false; continue; }
    if (parens) continue;     // :not(...) and friends hold a selector of their own
    if (c == ',') { if (prev_comma || prev_comb) return false; prev_comma = true; continue; }
    if (c == '>' || c == '+' || c == '~') { if (prev_comma || prev_comb) return false; prev_comb = true; continue; }
    if (std::isspace(c)) continue;
    const bool allowed = std::isalnum(c) || c >= 0x80 || c == '-' || c == '_' || c == '.' || c == '#' || c == '*' || c == ':' || c == '|' || c == '%' || c == '&';
    if (!allowed) return false;
    prev_comma = prev_comb = false;
  }
  return !quote && !parens && !brackets && !prev_comma && !prev_comb;
}

}  // namespace

CompiledSelectors compile_selectors(const std::string& selector) {
  CompiledSelectors out;
  out.valid = selector_text_well_formed(selector);
  if (!out.valid) return out;
  for (const std::string& one : split_selector_list(selector)) {
    Selector sel = parse_selector(one);
    if (!sel.parts.empty()) out.list.push_back(std::move(sel));
  }
  return out;
}

bool compiled_selectors_match(const Node& el, const CompiledSelectors& compiled) {
  for (const Selector& sel : compiled.list)
    if (matches(el, sel)) return true;
  return false;
}

// ------------------------------------------------------------------- @media --

// `calc(640px - 1px)` as a media feature value -- how a stylesheet writes "just
// under the breakpoint". Read as nothing, the whole condition was dropped and
// the query counted as satisfied, so a page's phone rules applied on a desktop.
bool media_calc_px(const std::string& q, size_t i, float& out) {
  if (q.compare(i, 5, "calc(") != 0) return false;
  size_t depth = 1, k = i + 5;
  while (k < q.size() && depth > 0) {
    if (q[k] == '(') ++depth;
    else if (q[k] == ')') --depth;
    ++k;
  }
  if (depth != 0) return false;
  const std::string inner = q.substr(i + 5, k - 1 - (i + 5));
  const Length l = eval_calc(inner, 16, 16);
  if (l.unit != Length::Unit::Px) return false;
  out = l.value + l.offset_px;
  return true;
}

bool media_applies(const std::string& query, int viewport_w) {
  if (query.empty()) return true;
  std::string q = lowered(query);
  // A sheet for paper or a screen reader is not this screen.
  if (q.find("print") != std::string::npos && q.find("screen") == std::string::npos) return false;
  if (q.find("speech") != std::string::npos) return false;
  // `@supports not (...)` is parsed as a media query. What it guards is the
  // fallback for a browser without the feature (Codex puts a background-image
  // beside every mask icon); this engine has the modern feature, so the
  // negated block never applies.
  if (starts_with(q, "not (") || starts_with(q, "not(")) return false;
  // The features this machine can answer for itself. A page asks these to find
  // out what it is being read on, and answering "yes, whatever you asked" made
  // it dress for the wrong one: a site with both a light and a dark block
  // applied BOTH, and the loser was decided by document order rather than by
  // what this screen is.
  //
  //   prefers-color-scheme  dark. The desktop's ground is #121212 and the UA
  //                         sheet is written for it.
  //   prefers-reduced-motion  no-preference. Nothing here animates yet, so
  //                         claiming a reduction would be a lie in the other
  //                         direction.
  //   orientation           landscape. The pane is a window on a desktop.
  //   hover / any-hover     hover. There is a pointer.
  //
  // A query naming one of these with a value this machine is not answers false;
  // anything still unrecognised is treated as satisfied, so a page's styles are
  // applied rather than dropped.
  struct Feature { const char* name; const char* answer; };
  static const Feature kFeatures[] = {
    {"prefers-color-scheme", "dark"},
    {"prefers-reduced-motion", "no-preference"},
    {"orientation", "landscape"},
    {"any-hover", "hover"},
    {"hover", "hover"},
  };
  for (const Feature& f : kFeatures) {
    size_t at2 = 0;
    for (;;) {
      size_t found = q.find(f.name, at2);
      if (found == std::string::npos) break;
      // `hover` is a substring of `any-hover`; only match a whole feature name.
      const bool left_ok = found == 0 || !(is_alpha(q[found - 1]) || q[found - 1] == '-');
      size_t after = found + std::strlen(f.name);
      at2 = after;
      if (!left_ok) continue;
      size_t colon2 = after;
      while (colon2 < q.size() && is_space(q[colon2])) ++colon2;
      if (colon2 >= q.size() || q[colon2] != ':') continue;   // `(hover)` alone: does it exist at all -- yes
      size_t v = colon2 + 1;
      while (v < q.size() && is_space(q[v])) ++v;
      size_t end = v;
      while (end < q.size() && (is_alpha(q[end]) || q[end] == '-')) ++end;
      if (end > v && q.compare(v, end - v, f.answer) != 0) return false;
    }
  }

  // Every min-width has to be satisfied and every max-width respected.
  size_t at = 0;
  for (;;) {
    size_t mn = q.find("min-width", at);
    if (mn == std::string::npos) break;
    size_t colon = q.find(':', mn);
    if (colon == std::string::npos) break;
    size_t i = colon + 1;
    while (i < q.size() && is_space(q[i])) ++i;
    float n = 0;
    size_t j = i;
    float calc_px = 0;
    if (media_calc_px(q, i, calc_px)) {
      if ((float)viewport_w < calc_px) return false;
    } else if (parse_number(q, j, n)) {
      std::string unit = q.substr(j, 3);
      float px = starts_with(unit, "em") ? n * 16.0f : n;
      if ((float)viewport_w < px) return false;
    }
    at = mn + 9;
  }
  at = 0;
  for (;;) {
    size_t mx = q.find("max-width", at);
    if (mx == std::string::npos) break;
    size_t colon = q.find(':', mx);
    if (colon == std::string::npos) break;
    size_t i = colon + 1;
    while (i < q.size() && is_space(q[i])) ++i;
    float n = 0;
    size_t j = i;
    float calc_px = 0;
    if (media_calc_px(q, i, calc_px)) {
      if ((float)viewport_w > calc_px) return false;
    } else if (parse_number(q, j, n)) {
      std::string unit = q.substr(j, 3);
      float px = starts_with(unit, "em") ? n * 16.0f : n;
      if ((float)viewport_w > px) return false;
    }
    at = mx + 9;
  }
  return true;
}

namespace {

struct RuleIndex {
  std::unordered_map<std::string, std::vector<uint32_t>> by_id, by_class, by_tag;
  std::vector<uint32_t> universal;
  std::unordered_map<std::string, std::vector<uint32_t>> before_by_id, before_by_class, before_by_tag;
  std::vector<uint32_t> before_universal;
  std::unordered_map<std::string, std::vector<uint32_t>> after_by_id, after_by_class, after_by_tag;
  std::vector<uint32_t> after_universal;
};

const RuleIndex& rule_index_of(const Stylesheet& sheet) {
  if (!sheet.rule_index || sheet.rule_index_n != sheet.rules.size()) {
    auto idx = std::make_shared<RuleIndex>();
    auto add = [](std::vector<uint32_t>& v, uint32_t i) { if (v.empty() || v.back() != i) v.push_back(i); };
    for (uint32_t i = 0; i < sheet.rules.size(); ++i) {
      for (const auto& sel : sheet.rules[i].selectors) {
        if (sel.parts.empty()) continue;
        const SimpleSelector& last = sel.parts.back();
        if (last.pseudo == SimpleSelector::PseudoEl::Before) {
          if (!last.id.empty()) add(idx->before_by_id[last.id], i);
          else if (!last.classes.empty()) add(idx->before_by_class[last.classes[0]], i);
          else if (!last.tag.empty()) add(idx->before_by_tag[last.tag], i);
          else add(idx->before_universal, i);
        } else if (last.pseudo == SimpleSelector::PseudoEl::After) {
          if (!last.id.empty()) add(idx->after_by_id[last.id], i);
          else if (!last.classes.empty()) add(idx->after_by_class[last.classes[0]], i);
          else if (!last.tag.empty()) add(idx->after_by_tag[last.tag], i);
          else add(idx->after_universal, i);
        } else if (!last.never) {
          if (!last.id.empty()) add(idx->by_id[last.id], i);
          else if (!last.classes.empty()) add(idx->by_class[last.classes[0]], i);
          else if (!last.tag.empty()) add(idx->by_tag[last.tag], i);
          else add(idx->universal, i);
        }
      }
    }
    sheet.rule_index = idx;
    sheet.rule_index_n = sheet.rules.size();
  }
  return *static_cast<const RuleIndex*>(sheet.rule_index.get());
}

// The rules that could apply to this element, in document order.
void candidate_rules(const Stylesheet& sheet, const Node& el, std::vector<uint32_t>& out) {
  const RuleIndex& idx = rule_index_of(sheet);
  out.assign(idx.universal.begin(), idx.universal.end());
  auto take = [&](const std::unordered_map<std::string, std::vector<uint32_t>>& m, const std::string& k) {
    auto it = m.find(k);
    if (it != m.end()) out.insert(out.end(), it->second.begin(), it->second.end());
  };
  const std::string& id = el.attr_ref("id");
  if (!id.empty()) {
    std::string id_low;
    id_low.reserve(id.size());
    for (char c : id) id_low += lower(c);
    take(idx.by_id, id_low);
  }
  take(idx.by_tag, el.tag);
  const std::string& cls = el.attr_ref("class");
  size_t i = 0;
  while (i < cls.size()) {
    while (i < cls.size() && is_space(cls[i])) ++i;
    const size_t start = i;
    while (i < cls.size() && !is_space(cls[i])) ++i;
    if (i > start) {
      std::string c_low;
      c_low.reserve(i - start);
      for (size_t k = start; k < i; ++k) c_low += lower(cls[k]);
      take(idx.by_class, c_low);
    }
  }
  if (out.size() > 1) {
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
  }
}

void candidate_rules_pseudo(const Stylesheet& sheet, const Node& el, bool after, std::vector<uint32_t>& out) {
  const RuleIndex& idx = rule_index_of(sheet);
  const auto& univ = after ? idx.after_universal : idx.before_universal;
  const auto& by_id = after ? idx.after_by_id : idx.before_by_id;
  const auto& by_tag = after ? idx.after_by_tag : idx.before_by_tag;
  const auto& by_class = after ? idx.after_by_class : idx.before_by_class;
  out.assign(univ.begin(), univ.end());
  auto take = [&](const std::unordered_map<std::string, std::vector<uint32_t>>& m, const std::string& k) {
    auto it = m.find(k);
    if (it != m.end()) out.insert(out.end(), it->second.begin(), it->second.end());
  };
  const std::string& id = el.attr_ref("id");
  if (!id.empty()) {
    std::string id_low;
    id_low.reserve(id.size());
    for (char c : id) id_low += lower(c);
    take(by_id, id_low);
  }
  take(by_tag, el.tag);
  const std::string& cls = el.attr_ref("class");
  size_t i = 0;
  while (i < cls.size()) {
    while (i < cls.size() && is_space(cls[i])) ++i;
    const size_t start = i;
    while (i < cls.size() && !is_space(cls[i])) ++i;
    if (i > start) {
      std::string c_low;
      c_low.reserve(i - start);
      for (size_t k = start; k < i; ++k) c_low += lower(cls[k]);
      take(by_class, c_low);
    }
  }
  if (out.size() > 1) {
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
  }
}

bool rule_media_ok(const Rule& r, int viewport_w) {
  if (r.media_vw != viewport_w) { r.media_ok = media_applies(r.media, viewport_w); r.media_vw = viewport_w; }
  return r.media_ok;
}

}  // namespace

// ------------------------------------------------------------------- parser --

Stylesheet parse_css(const std::string& source) {
  Stylesheet sheet;

  // Comments out first: a `/* { */` inside a selector otherwise opens a block.
  std::string src;
  src.reserve(source.size());
  for (size_t i = 0; i < source.size();) {
    if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '*') {
      size_t end = source.find("*/", i + 2);
      i = end == std::string::npos ? source.size() : end + 2;
      src += ' ';
      continue;
    }
    src += source[i++];
  }

  // A stack of enclosing `@media` queries, so a rule inside two nested ones
  // carries both.
  std::vector<std::string> media_stack;
  std::string current_media;
  auto rebuild_media = [&]() {
    current_media.clear();
    for (const auto& m : media_stack) {
      if (!current_media.empty()) current_media += " and ";
      current_media += m;
    }
  };

  size_t i = 0;
  while (i < src.size()) {
    while (i < src.size() && (is_space(src[i]) || src[i] == ';')) ++i;
    if (i >= src.size()) break;

    if (src[i] == '}') {
      if (!media_stack.empty()) {
        media_stack.pop_back();
        rebuild_media();
      }
      ++i;
      continue;
    }

    // An at-rule. `@media` and `@supports` hold rules, so step INTO their
    // block; everything else (`@import`, `@font-face`, `@keyframes`, …) is
    // skipped whole, brace-balanced.
    if (src[i] == '@') {
      size_t name_end = i + 1;
      while (name_end < src.size() && !is_space(src[name_end]) && src[name_end] != '{' && src[name_end] != ';')
        ++name_end;
      std::string at_name = lowered(src.substr(i + 1, name_end - i - 1));
      size_t brace = src.find('{', i);
      size_t semi = src.find(';', i);
      if (semi != std::string::npos && (brace == std::string::npos || semi < brace)) {
        i = semi + 1;   // a statement at-rule: `@import url(...)` etc.
        continue;
      }
      if (brace == std::string::npos) break;
      if (at_name == "media" || at_name == "supports") {
        media_stack.push_back(trimmed(src.substr(name_end, brace - name_end)));
        rebuild_media();
        i = brace + 1;
        continue;
      }
      // Skip the whole block, counting braces so `@keyframes` with its nested
      // stops does not leave the parser inside it.
      int depth = 0;
      size_t j = brace;
      for (; j < src.size(); ++j) {
        if (src[j] == '{') ++depth;
        else if (src[j] == '}' && --depth == 0) { ++j; break; }
      }
      i = j;
      continue;
    }

    // A qualified rule: selector list, then a declaration block.
    size_t brace = src.find('{', i);
    if (brace == std::string::npos) break;
    std::string selector_text = src.substr(i, brace - i);
    size_t close = brace + 1;
    int depth = 1;
    for (; close < src.size(); ++close) {
      if (src[close] == '{') ++depth;
      else if (src[close] == '}' && --depth == 0) break;
    }
    std::string body = src.substr(brace + 1, (close < src.size() ? close : src.size()) - brace - 1);
    i = close < src.size() ? close + 1 : src.size();

    Rule rule;
    rule.media = current_media;
    rule.source = trimmed(selector_text);
    // The selector list, split only on its top-level commas.
    for (const std::string& one : split_selector_list(selector_text)) {
      Selector sel = parse_selector(one);
      if (!sel.parts.empty()) rule.selectors.push_back(std::move(sel));
    }
    if (rule.selectors.empty()) continue;

    // The declarations. Split on top-level semicolons so `url(data:...;base64)`
    // does not end one early.
    size_t d = 0;
    int paren = 0;
    std::string decl;
    auto push_decl = [&]() {
      std::string one = trimmed(decl);
      decl.clear();
      if (one.empty()) return;
      size_t colon = one.find(':');
      if (colon == std::string::npos) return;
      Declaration dec;
      dec.property = lowered(trimmed(one.substr(0, colon)));
      std::string value = trimmed(one.substr(colon + 1));
      std::string low = lowered(value);
      size_t bang = low.rfind('!');
      if (bang != std::string::npos && trimmed(low.substr(bang + 1)) == "important") {
        dec.important = true;
        value = trimmed(value.substr(0, bang));
      }
      dec.value = value;
      if (!dec.property.empty() && !dec.value.empty()) rule.decls.push_back(std::move(dec));
    };
    for (; d < body.size(); ++d) {
      char c = body[d];
      if (c == '(') ++paren;
      if (c == ')' && paren > 0) --paren;
      if (c == ';' && paren == 0) {
        push_decl();
        continue;
      }
      decl += c;
    }
    push_decl();
    if (!rule.decls.empty()) sheet.rules.push_back(std::move(rule));
  }
  return sheet;
}

// ------------------------------------------------------------ the UA sheet --

const Stylesheet& ua_stylesheet(bool light_ground) {
  // Built once. This is what makes an unstyled document still read like a
  // document: it is the engine's own opinion about what HTML means, written in
  // the same language a page is, so it goes through the same cascade and a page
  // can override any of it.
  //
  // `color` is set on links and NOWHERE else -- not even on html/body. It used to be
  // set on every heading, on code, th, caption, blockquote and figcaption --
  // near-white values for this machine's dark ground. A page that sets its own
  // light background inherits its own colour into the body and then loses every
  // one of those elements, because a UA rule naming h1 beats a colour inherited
  // from the author's body: on any white page, every heading rendered #f2f2f2
  // on white. Inheritance already does this job in both directions -- an
  // unstyled page still gets light headings from body, and a styled one gets
  // the author's.
  //
  // html/body lost theirs too, and for a second reason: the INITIAL colour is
  // set on the initial containing block in layout(), where it can be chosen
  // against the ground the page turns out to have. A UA rule naming body would
  // beat that, and did -- see the ink floor in web_layout.cpp.
  static const Stylesheet* sheet = nullptr;
  static const Stylesheet* light_sheet = nullptr;
  if (light_ground ? (light_sheet != nullptr) : (sheet != nullptr))
    return light_ground ? *light_sheet : *sheet;
  static const char* kDefault = R"CSS(
html, body { display: block; background: #121212; font-size: 15px; }
/* The frame around a plain document, and the one an application shell turns
   off with `body { margin: 0 }`. It lives here, in the UA sheet, because that
   is the only place an author can override it -- the layout used to apply an
   18px gutter of its own that no stylesheet could reach. */
body { margin: 8px; }
head, script, style, meta, link, title, noscript, template, base, param, source, track { display: none; }
div, section, article, aside, header, footer, nav, main, figure, figcaption, address,
hgroup, details, summary, form, fieldset, dl, dt, dd, blockquote, pre, table, caption,
tbody, thead, tfoot, colgroup, col, iframe, hr, p, ul, ol, li { display: block; }
video, audio, canvas, img, svg { display: inline-block; }
audio:not([controls]) { display: none; }
video { object-fit: contain; background: #000000; }
p { margin: 12px 0 12px 0; }
h1 { display: block; font-size: 30px; font-weight: bold; margin: 22px 0 14px 0; }
h2 { display: block; font-size: 24px; font-weight: bold; margin: 20px 0 12px 0; }
h3 { display: block; font-size: 19px; font-weight: bold; margin: 18px 0 10px 0; }
h4 { display: block; font-size: 16px; font-weight: bold; margin: 16px 0 8px 0; }
h5, h6 { display: block; font-size: 15px; font-weight: bold; margin: 14px 0 8px 0; }
b, strong, th { font-weight: bold; }
i, em, cite, var, dfn, address { font-style: italic; }
u, ins { text-decoration: underline; }
s, strike, del { text-decoration: line-through; }
small { font-size: 12px; }
big { font-size: 18px; }
code, kbd, samp, tt { font-family: monospace; font-size: 14px; }
pre { font-family: monospace; white-space: pre; margin: 12px 0 12px 0; padding: 10px;
      background: #1b1b1b; border: 1px solid #2c2c2c; color: #dcdcdc; }
a { color: $LINK$; text-decoration: underline; }
ul, ol { margin: 12px 0 12px 0; padding-left: 30px; }
li { display: list-item; margin: 3px 0 3px 0; }
ul ul, ol ol, ul ol, ol ul { margin: 2px 0 2px 0; }
blockquote { margin: 12px 0 12px 26px; padding-left: 12px; border-left: 3px solid $RULE$; }
hr { display: block; margin: 14px 0 14px 0; border-top: 1px solid $RULE$; }
/* `<center>` is a block. Without it the element flowed inline, so its text never
   centred and two of them ran together on one line -- google.com's buttons sit
   in one. */
center { display: block; }
/* `text-align: left` here is not cosmetic: it stops an enclosing `<center>`
   from centring every cell's text. Hacker News wraps its whole page in one,
   and without this rule every headline came out centred in its column. A
   `<td align=center>` still centres, because a presentational attribute
   outranks this sheet. */
table { display: table; margin: 12px 0 12px 0; text-align: left; }
tr { display: table-row; }
/* A cell has NO border of its own. Every browser's default sheet works this
   way, and giving one to td/th drew a grid over every page that uses a table
   for layout -- which in 1998 and on Hacker News today is most of them. The
   grid comes back when the author asks for it in CSS, or with `<table
   border=N>`, which presentational_hints() below turns into a real border --
   a selector cannot tell `border="1"` from `border="0"`, and Hacker News
   writes the second one. */
td, th { display: table-cell; padding: 4px 8px 4px 8px; }
caption { display: block; font-style: italic; margin: 4px 0 4px 0; }
dt { font-weight: bold; margin: 8px 0 2px 0; }
dd { margin: 0 0 8px 26px; }
figure { margin: 14px 0 14px 0; }
figcaption { font-size: 13px; margin: 4px 0 0 0; }
mark { background: $MARKBG$; color: $MARKINK$; }
button, input, select, textarea { display: inline-block; border: 1px solid $CTLEDGE$; padding: 1px 2px 1px 2px;
      background: $FIELDBG$; color: $FIELDINK$; border-radius: 2px; text-align: left; }
select { padding: 1px 4px 1px 4px; border-radius: 3px; }
/* A button is not a field. Every browser paints the two differently -- a field
   is the page's paper, a button is raised off it -- and lumping them together
   made a submit button look like an empty text box. */
button, input[type=submit], input[type=button], input[type=reset] { background: $BTNFACE$; padding: 1px 6px 1px 6px;
      border-radius: 3px; text-align: center; }
/* Under the pointer a button darkens a step, as Chrome's do. */
button:hover, input[type=submit]:hover, input[type=button]:hover, input[type=reset]:hover { background: $BTNHOVER$; }
/* A disabled control is there but out of reach: Chrome mutes its paper, edge and ink. */
input:disabled, textarea:disabled, select:disabled, button:disabled { background: $DISBG$; color: $DISINK$;
      border-color: $DISEDGE$; }
/* A checkbox or radio is a 13px mark the engine draws (Chrome's size and margins), not a framed field. */
input[type=checkbox], input[type=radio] { border: 0; padding: 0; background: transparent; margin: 3px 3px 3px 4px; }
input[type=radio] { margin: 3px 3px 0 5px; }
/* A hidden input is NOT a control with `display: inline-block` from the rule
   above: it is not rendered at all. Without this every one of them was a box
   on the page, sized to its VALUE -- google.com carries six, and the 44
   characters of one signing token stretched the row holding "I'm Feeling
   Lucky" to 612 pixels. */
input[type=hidden] { display: none; }
img { display: inline-block; }
abbr { text-decoration: underline; }
sup, sub { font-size: 11px; }
nav a { text-decoration: none; }
)CSS";
  // Every value in the sheet that has to change with the ground is written as
  // a $TOKEN$ and filled in here, for BOTH grounds. It used to be a list of
  // find-and-replace pairs over the dark text, which is a booby trap: editing
  // the padding on the button rule made its colour swap stop matching, in
  // silence, and every button on every light page went back to being a dark
  // slab. A token that is not substituted is a parse error you can see, not a
  // colour that is quietly wrong.
  static const struct { const char* token; const char* dark; const char* light; } kInk[] = {
      {"$LINK$",     "#6fb6ff", "#0b57a4"},   // a visited-blue that reads on either ground
      {"$RULE$",     "#3a3a3a", "#c8c8c8"},   // hr, blockquote's edge
      {"$MARKBG$",   "#4a4416", "#fff1a8"},
      {"$MARKINK$",  "#f7f0c0", "#2b2600"},
      // A field is paper with a rule round it; a button is the raised face.
      // These are Chrome's own values for each ground.
      {"$FIELDBG$",  "#202020", "#ffffff"},
      {"$FIELDINK$", "#dcdcdc", "#000000"},
      {"$CTLEDGE$",  "#3d3d3d", "#767676"},
      {"$BTNFACE$",  "#2f2f2f", "#efefef"},
      {"$BTNHOVER$", "#3a3a3a", "#e5e5e5"},
      {"$DISBG$",    "#262626", "#fafafa"},
      {"$DISINK$",   "#8a8a8a", "#6d6d6d"},
      {"$DISEDGE$",  "#333333", "#d6d6d6"},
  };
  auto fill = [](bool light) {
    std::string out = kDefault;
    for (const auto& ink : kInk) {
      const std::string tok = ink.token;
      const std::string val = light ? ink.light : ink.dark;
      for (size_t at = out.find(tok); at != std::string::npos; at = out.find(tok, at + val.size()))
        out.replace(at, tok.size(), val);
    }
    return out;
  };
  if (light_ground) {
    static Stylesheet built_light = parse_css(fill(true));
    light_sheet = &built_light;
    return built_light;
  }
  static Stylesheet built_dark = parse_css(fill(false));
  sheet = &built_dark;
  return built_dark;
}

// ------------------------------------------------- custom properties, var() --

const std::string* CustomProps::find(const std::string& name) const {
  for (const auto& e : entries)
    if (e.first == name) return &e.second;
  return nullptr;
}

namespace {

// `var(--name)` / `var(--name, fallback)`, substituted before a value is
// parsed. A themed application writes every colour, radius and spacing this
// way; without substitution each one resolved to nothing and the page came out
// unpainted, which reads as the engine having no CSS at all.
//
// The fallback is itself a value that may contain a var(), and a substituted
// value may contain another -- so this runs to a fixed point, bounded because a
// cycle must not hang the layout.
std::string substitute_vars(const std::string& value, const CustomProps* props, int depth = 0) {
  if (depth > 8 || value.find("var(") == std::string::npos) return value;
  std::string out;
  out.reserve(value.size());
  size_t i = 0;
  while (i < value.size()) {
    // Only at a token boundary: `--x` inside a string or a longer identifier
    // is not a var() call.
    if (value.compare(i, 4, "var(") == 0 &&
        (i == 0 || !(is_alpha(value[i - 1]) || value[i - 1] == '-'))) {
      size_t j = i + 4, depth_p = 1;
      for (; j < value.size(); ++j) {
        if (value[j] == '(') ++depth_p;
        else if (value[j] == ')' && --depth_p == 0) break;
      }
      const std::string inner = value.substr(i + 4, j - i - 4);
      // The name runs to the first top-level comma; the rest is the fallback.
      size_t comma = std::string::npos;
      int d2 = 0;
      for (size_t k = 0; k < inner.size(); ++k) {
        if (inner[k] == '(') ++d2;
        else if (inner[k] == ')') --d2;
        else if (inner[k] == ',' && d2 == 0) { comma = k; break; }
      }
      const std::string name = trimmed(comma == std::string::npos ? inner : inner.substr(0, comma));
      const std::string fallback = comma == std::string::npos ? std::string()
                                                             : trimmed(inner.substr(comma + 1));
      const std::string* found = props ? props->find(name) : nullptr;
      // A declared-but-empty custom property is a real value (the empty one),
      // so `found` is checked rather than its contents.
      out += substitute_vars(found ? *found : fallback, props, depth + 1);
      i = (j < value.size()) ? j + 1 : value.size();
      continue;
    }
    out += value[i++];
  }
  return out;
}

}  // namespace

// ------------------------------------------------------------- the cascade --

namespace {

// The properties that come from the parent when nobody says otherwise. The
// list is short on purpose: getting inheritance wrong on `background` paints
// every child of a coloured box, which is much more visible than missing it.
void inherit(Style& s, const Style& parent) {
  s.color = parent.color;
  s.font_px = parent.font_px;
  s.bold = parent.bold;
  s.italic = parent.italic;
  s.underline = parent.underline;
  s.strike = parent.strike;
  s.mono = parent.mono;
  s.serif = parent.serif;
  s.text_align = parent.text_align;
  s.white_space = parent.white_space;
  s.line_height = parent.line_height;
  s.line_height_factor = parent.line_height_factor;
  s.list_bullet = parent.list_bullet;
  s.letter_spacing = parent.letter_spacing;
  s.text_case = parent.text_case;
  s.accent = parent.accent;
  // Custom properties inherit, and share the parent's list until an element
  // declares one of its own.
  s.custom = parent.custom;
}


// The first family in a font-family list that names a face we can draw wins,
// as in a browser: `Georgia, serif` is serif, `Arial, monospace` is sans.
// An unknown name (a webfont we never fetched) falls through to the next one.
static void apply_family_list(Style& s, const std::string& list) {
  size_t pos = 0;
  while (pos <= list.size()) {
    size_t comma = list.find(',', pos);
    std::string fam = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    pos = comma == std::string::npos ? list.size() + 1 : comma + 1;
    auto has = [&](const char* t) { return fam.find(t) != std::string::npos; };
    if (has("mono") || has("courier") || has("consolas") || has("menlo") || has("ui-monospace") ||
        has("code")) { s.mono = true; s.serif = false; return; }
    if (has("sans") || has("arial") || has("helvetica") || has("system-ui") || has("ui-rounded") ||
        has("segoe") || has("roboto") || has("verdana") || has("tahoma") || has("inter") ||
        has("-apple-system") || has("blinkmacsystemfont") || has("trebuchet")) {
      s.mono = false; s.serif = false; return;
    }
    if (has("serif") || has("georgia") || has("times") || has("garamond") || has("palatino") ||
        has("cambria") || has("charter") || has("book")) { s.mono = false; s.serif = true; return; }
  }
}

void apply_declaration(Style& s, const Declaration& d, const Style& parent, int viewport_w) {
  const std::string& p = d.property;
  const std::string& v = d.value;
  std::string lv_buf;
  auto get_lv = [&]() -> const std::string& {
    if (lv_buf.empty() && !v.empty()) lv_buf = lowered(v);
    return lv_buf;
  };
  const int root_px = 15;
  const int em = parent.font_px > 0 ? parent.font_px : 15;

  auto iequals = [](const std::string& s1, const char* s2) {
    if (s1.size() != std::strlen(s2)) return false;
    for (size_t i = 0; i < s1.size(); ++i) {
      if (lower(s1[i]) != s2[i]) return false;
    }
    return true;
  };

  if (iequals(v, "inherit") || iequals(v, "initial") || iequals(v, "unset")) {
    const std::string& lv = get_lv();
    static const Style def{};
    bool is_inherited = (p == "color" || p == "font" || p == "font-family" || p == "font-size" ||
                         p == "font-weight" || p == "font-style" || p == "line-height" ||
                         p == "letter-spacing" || p == "text-align" || p == "text-transform" ||
                         p == "white-space" || p == "list-style" || p == "list-style-type" ||
                         p == "cursor" || p == "visibility" || p == "accent-color");
    const Style& src = (lv == "inherit" || (lv == "unset" && is_inherited)) ? parent : def;
    if (p == "color") { s.color = src.color; return; }
    if (p == "accent-color") { s.accent = src.accent; return; }
    if (p == "appearance" || p == "-webkit-appearance") { s.appearance_none = src.appearance_none; return; }
    if (p == "font-size") { s.font_px = src.font_px; return; }
    if (p == "font-weight") { s.bold = src.bold; return; }
    if (p == "font-style") { s.italic = src.italic; return; }
    if (p == "font-family") { s.mono = src.mono; return; }
    if (p == "line-height") { s.line_height = src.line_height; s.line_height_factor = src.line_height_factor; return; }
    if (p == "text-align") { s.text_align = src.text_align; return; }
    if (p == "white-space") { s.white_space = src.white_space; return; }
    if (p == "letter-spacing") { s.letter_spacing = src.letter_spacing; return; }
    if (p == "text-transform") { s.text_case = src.text_case; return; }
    if (p == "list-style" || p == "list-style-type") { s.list_bullet = src.list_bullet; return; }
    if (p == "text-decoration" || p == "text-decoration-line") { s.underline = src.underline; s.strike = src.strike; return; }
    if (p == "background" || p == "background-color") {
      s.background = src.background;
      s.has_background = src.has_background;
      return;
    }
    if (p == "display") { s.display = src.display; return; }
    if (p == "position") { s.position = src.position; return; }
    if (p == "width") { s.width = src.width; return; }
    if (p == "height") { s.height = src.height; return; }
    if (p == "min-width") { s.min_width = src.min_width; return; }
    if (p == "max-width") { s.max_width = src.max_width; return; }
    if (p == "margin") {
      for (int i = 0; i < 4; ++i) { s.margin[i] = src.margin[i]; s.margin_auto[i] = src.margin_auto[i]; }
      return;
    }
    if (p == "padding") {
      for (int i = 0; i < 4; ++i) s.padding[i] = src.padding[i];
      return;
    }
    if (p == "border") {
      for (int i = 0; i < 4; ++i) s.border[i] = src.border[i];
      s.border_color = src.border_color;
      return;
    }
    if (p == "border-radius") { s.radius = src.radius; return; }
    if (p == "box-sizing") { s.border_box = src.border_box; return; }
    return;
  }

  if (p == "position") {
    const std::string a = lowered(trimmed(v));
    if (a == "static") s.position = Position::Static;
    else if (a == "relative") s.position = Position::Relative;
    else if (a == "absolute") s.position = Position::Absolute;
    else if (a == "fixed") s.position = Position::Fixed;
    else if (a == "sticky" || a == "-webkit-sticky") s.position = Position::Sticky;
    return;
  }
  if (p == "top" || p == "right" || p == "bottom" || p == "left") {
    const int which = p == "top" ? 0 : p == "right" ? 1 : p == "bottom" ? 2 : 3;
    s.inset[which] = parse_length(v, em, root_px);
    return;
  }
  if (p == "inset") {
    Length four[4];
    auto parts = split_parts(trimmed(v));
    for (size_t i = 0; i < 4; ++i) {
      const size_t from = parts.size() == 1 ? 0 : parts.size() == 2 ? (i % 2) : i < parts.size() ? i : (i == 3 ? 1 : i);
      if (from < parts.size()) four[i] = parse_length(parts[from], em, root_px);
    }
    for (int i = 0; i < 4; ++i) s.inset[i] = four[i];
    return;
  }
  if (p == "mask-image" || p == "-webkit-mask-image") {
    s.mask_image.clear();
    const size_t u = v.find("url(");
    if (u != std::string::npos) {
      size_t a = u + 4, b = v.find(')', a);
      if (b != std::string::npos) {
        std::string inner = trimmed(v.substr(a, b - a));
        if (inner.size() >= 2 && (inner.front() == '"' || inner.front() == '\'') && inner.back() == inner.front())
          inner = inner.substr(1, inner.size() - 2);
        s.mask_image = inner;
      }
    }
    return;
  }
  if (p == "float") {
    const std::string a = lowered(trimmed(v));
    if (a == "left" || a == "inline-start") s.float_side = FloatSide::Left;
    else if (a == "right" || a == "inline-end") s.float_side = FloatSide::Right;
    else if (a == "none") s.float_side = FloatSide::None;
    return;
  }
  if (p == "clear") {
    const std::string a = lowered(trimmed(v));
    if (a == "left" || a == "inline-start") s.clear_side = ClearSide::Left;
    else if (a == "right" || a == "inline-end") s.clear_side = ClearSide::Right;
    else if (a == "both") s.clear_side = ClearSide::Both;
    else if (a == "none") s.clear_side = ClearSide::None;
    return;
  }
  if (p == "z-index") {
    const std::string a = lowered(trimmed(v));
    if (a == "auto") { s.has_z = false; s.z_index = 0; return; }
    size_t i = 0;
    float n = 0;
    if (!parse_number(a, i, n)) return;
    s.z_index = round_to_int(n);
    s.has_z = true;
    return;
  }
  if (p == "overflow" || p == "overflow-x" || p == "overflow-y") {
    const std::string a = lowered(trimmed(v));
    auto one = [](const std::string& t) {
      if (t == "hidden" || t == "clip") return Overflow::Hidden;
      if (t == "scroll") return Overflow::Scroll;
      if (t == "auto" || t == "overlay") return Overflow::Auto;
      return Overflow::Visible;
    };
    if (p == "overflow") {
      auto parts = split_parts(a);
      if (parts.empty()) return;
      s.overflow_x = one(parts[0]);
      s.overflow_y = one(parts.size() > 1 ? parts[1] : parts[0]);
    } else if (p == "overflow-x") {
      s.overflow_x = one(a);
    } else {
      s.overflow_y = one(a);
    }
    return;
  }
  // `place-items: a b` is `align-items: a; justify-items: b`, one value for both.
  if (p == "place-items" || p == "place-self") {
    auto parts = split_parts(v);
    if (parts.empty()) return;
    Declaration a = d, j = d;
    a.property = p == "place-items" ? "align-items" : "align-self";
    j.property = p == "place-items" ? "justify-items" : "justify-self";
    a.value = parts[0];
    j.value = parts.size() > 1 ? parts[1] : parts[0];
    apply_declaration(s, a, parent, viewport_w);
    apply_declaration(s, j, parent, viewport_w);
    return;
  }
  if (p == "align-items" || p == "align-self" || p == "justify-items" || p == "justify-self") {
    const std::string a = lowered(trimmed(v));
    AlignItems got = AlignItems::Stretch;
    if (a == "flex-start" || a == "start" || a == "self-start") got = AlignItems::Start;
    else if (a == "center") got = AlignItems::Center;
    else if (a == "flex-end" || a == "end" || a == "self-end") got = AlignItems::End;
    else if (a == "baseline" || a == "first" || a == "last") got = AlignItems::Baseline;
    else if (a == "stretch" || a == "normal") got = AlignItems::Stretch;
    else if (a == "left" || a == "right") {}
    else return;
    // `left`/`right` are the physical spellings of start/end, which a
    // left-to-right page cannot tell apart.
    if (a == "left") got = AlignItems::Start;
    else if (a == "right") got = AlignItems::End;
    if (p == "align-self") s.align_self = got;
    else if (p == "justify-self") s.justify_self = got;
    else if (p == "justify-items") s.justify_items = got;
    else s.align_items = got;
    return;
  }
  if (p == "justify-content") {
    const std::string a = lowered(trimmed(v));
    if (a == "flex-start" || a == "start" || a == "left" || a == "normal") s.justify_content = Justify::Start;
    else if (a == "center") s.justify_content = Justify::Center;
    else if (a == "flex-end" || a == "end" || a == "right") s.justify_content = Justify::End;
    else if (a == "space-between") s.justify_content = Justify::SpaceBetween;
    else if (a == "space-around") s.justify_content = Justify::SpaceAround;
    else if (a == "space-evenly") s.justify_content = Justify::SpaceEvenly;
    return;
  }
  // `place-items` / `place-content`: the two axes in one, align first.
  if (p == "place-items" || p == "place-content") {
    auto parts = split_parts(trimmed(v));
    if (parts.empty()) return;
    Declaration a{p == "place-items" ? "align-items" : "align-content", parts[0], d.important};
    apply_declaration(s, a, parent, viewport_w);
    Declaration j{"justify-content", parts.size() > 1 ? parts[1] : parts[0], d.important};
    apply_declaration(s, j, parent, viewport_w);
    return;
  }
  if (p == "flex-direction") {
    const std::string lv = lowered(trimmed(v));
    s.flex_dir = (lv == "column" || lv == "column-reverse") ? FlexDir::Column : FlexDir::Row;
    s.flex_reverse = lv.find("reverse") != std::string::npos;
    return;
  }
  if (p == "flex-grow") {
    size_t at = 0; float n = 0;
    if (parse_number(lowered(trimmed(v)), at, n) && n >= 0) { s.flex_grow = n; s.flex_none = false; }
    return;
  }
  if (p == "flex-shrink") {
    size_t at = 0; float n = 0;
    if (parse_number(lowered(trimmed(v)), at, n) && n >= 0) s.flex_shrink = n;
    return;
  }
  if (p == "flex-basis") { s.flex_basis = parse_length(v, em, root_px); return; }
  if (p == "order") {
    size_t at = 0; float n = 0;
    if (parse_number(lowered(trimmed(v)), at, n)) s.order = (int)std::max(-100000.0f, std::min(100000.0f, n));
    return;
  }
  if (p == "align-content") {
    const std::string a = lowered(trimmed(v));
    if (a == "normal" || a == "stretch") s.align_content = 0;
    else if (a == "flex-start" || a == "start") s.align_content = 1;
    else if (a == "center") s.align_content = 2;
    else if (a == "flex-end" || a == "end") s.align_content = 3;
    else if (a == "space-between") s.align_content = 4;
    else if (a == "space-around") s.align_content = 5;
    else if (a == "space-evenly") s.align_content = 6;
    return;
  }
  if (p == "flex") {
    // `flex: <grow> [<shrink>] [<basis>]`, or none / auto / initial. A single
    // unitless number is grow with a ZERO basis (`flex: 1` = `1 1 0%`).
    const std::string lv = lowered(trimmed(v));
    float grow = 0.0f, shrink = 1.0f;
    Length basis;             // auto
    if (lv == "none") { shrink = 0.0f; }
    else if (lv == "auto") { grow = 1.0f; }
    else if (lv == "initial") {}
    else {
      int nums = 0;
      bool have_basis = false;
      for (const std::string& part : split_parts(lv)) {
        size_t at = 0; float n = 0;
        if (parse_number(part, at, n) && at == part.size()) {
          if (nums == 0) grow = n; else if (nums == 1) shrink = n;
          ++nums;
        } else {
          basis = parse_length(part, em, root_px);
          have_basis = true;
        }
      }
      if (nums > 0 && !have_basis) { basis.unit = Length::Unit::Px; basis.value = 0; }
    }
    s.flex_grow = grow; s.flex_shrink = shrink; s.flex_basis = basis;
    s.flex_none = grow == 0.0f && shrink == 0.0f;
    return;
  }
  const std::string& lv = get_lv();
  if (p == "display") {
    // A later `display` wins outright, including its inline-ness: a rule that
    // says `block` must undo an earlier `inline-flex`, not keep half of it.
    s.inline_level = false;
    s.flow_root = lv == "flow-root";
    if (lv == "none") { s.display = Display::None; s.hidden = true; }
    else if (lv == "flex") s.display = Display::Flex;
    // handled below: flex-direction, flex, flex-grow
    else if (lv == "grid" || lv == "inline-grid") {
      s.display = Display::Grid;
      s.inline_level = lv[0] == 'i';
    }
    else if (lv == "block" || lv == "flow-root") s.display = Display::Block;
    else if (lv == "inline") s.display = Display::Inline;
    else if (lv == "inline-flex") { s.display = Display::Flex; s.inline_level = true; }
    else if (lv == "inline-block") s.display = Display::InlineBlock;
    else if (lv == "list-item") s.display = Display::ListItem;
    else if (lv == "table" || lv == "inline-table") s.display = Display::Table;
    else if (lv == "table-row") s.display = Display::TableRow;
    else if (lv == "table-cell") s.display = Display::TableCell;
    else if (lv == "table-row-group" || lv == "table-header-group" || lv == "table-footer-group")
      s.display = Display::Block;
    else if (lv == "contents") s.display = Display::Block;
    if (lv != "none") s.hidden = false;
    return;
  }
  if (p == "visibility") {
    if (lv == "hidden" || lv == "collapse") s.hidden = true;
    return;
  }
  if (p == "appearance" || p == "-webkit-appearance" || p == "-moz-appearance") {
    s.appearance_none = lv == "none";
    return;
  }
  if (p == "accent-color") {
    uint32_t c = 0;
    if (lv == "auto") s.accent = 0;
    else if (lv == "currentcolor") s.accent = s.color;
    else if (parse_colour(v, c) && (c >> 24)) s.accent = c;
    return;
  }
  if (p == "color") {
    uint32_t c = 0;
    if (lv == "currentcolor") s.color = parent.color;
    else if (parse_colour(v, c) && (c >> 24)) s.color = c;
    return;
  }
  if (p == "aspect-ratio") {
    size_t slash = lv.find('/');
    if (slash != std::string::npos) {
      float w = (float)std::atof(lv.substr(0, slash).c_str());
      float h = (float)std::atof(lv.substr(slash + 1).c_str());
      if (w > 0 && h > 0) s.aspect_ratio = w / h;
    } else {
      float r = (float)std::atof(lv.c_str());
      if (r > 0) s.aspect_ratio = r;
    }
    return;
  }
  if (p == "object-fit") {
    if (lv == "contain") s.object_fit = Style::ObjectFit::Contain;
    else if (lv == "cover") s.object_fit = Style::ObjectFit::Cover;
    else if (lv == "none") s.object_fit = Style::ObjectFit::None;
    else if (lv == "scale-down") s.object_fit = Style::ObjectFit::ScaleDown;
    else s.object_fit = Style::ObjectFit::Fill;
    return;
  }
  if (p == "background-repeat") { s.bg_repeat = css_repeat(css_lower_parts(v), 3); return; }
  if (p == "background-size") { css_bg_size(s, css_lower_parts(v), em, root_px); return; }
  if (p == "background-position") {
    css_position(css_lower_parts(v), em, root_px, s.bg_pos_x, s.bg_pos_y, s.bg_off_x, s.bg_off_y);
    return;
  }
  if (p == "object-position") {
    float ox = 0, oy = 0;   // a px offset in object-position is rare enough to drop
    css_position(css_lower_parts(v), em, root_px, s.object_pos_x, s.object_pos_y, ox, oy);
    return;
  }
  if (p == "background" || p == "background-color" || p == "background-image") {
    if (p == "background") css_background_shorthand(s, v, em, root_px);
    if (p == "background-image") {
      std::string u;
      s.bg_image.clear();
      if (css_url_arg(v, u)) s.bg_image = u;
    }
    if (lv.find("linear-gradient(") != std::string::npos) {
      size_t open = lv.find("linear-gradient(");
      size_t close = lv.find(')', open);
      if (close != std::string::npos) {
        std::string args = v.substr(open + 16, close - (open + 16));
        auto parts = split_parts(args);
        std::vector<uint32_t> colors;
        for (const auto& part : parts) {
          uint32_t gc = 0;
          if (parse_colour(part, gc) && (gc >> 24)) colors.push_back(gc);
        }
        if (colors.size() >= 2) {
          s.has_gradient = true;
          s.gradient_start = colors[0];
          s.gradient_end = colors.back();
          s.has_background = true;
          s.background = colors[0];
          return;
        }
      }
    }
    uint32_t c = 0;
    // `background: #fff url(x) no-repeat` — find the part that is a colour.
    for (const auto& part : split_parts(v)) {
      if (parse_colour(part, c)) {
        if ((c >> 24) == 0) { s.has_background = false; return; }
        s.background = c;
        s.has_background = true;
        return;
      }
    }
    if (lv == "none" || lv == "transparent") s.has_background = false;
    return;
  }
  if (p == "font-size") {
    if (lv == "smaller") { s.font_px = em * 5 / 6; return; }
    if (lv == "larger") { s.font_px = em * 6 / 5; return; }
    if (lv == "xx-small") { s.font_px = 9; return; }
    if (lv == "x-small") { s.font_px = 11; return; }
    if (lv == "small") { s.font_px = 13; return; }
    if (lv == "medium") { s.font_px = 15; return; }
    if (lv == "large") { s.font_px = 18; return; }
    if (lv == "x-large") { s.font_px = 22; return; }
    if (lv == "xx-large") { s.font_px = 28; return; }
    Length l = parse_length(v, em, root_px);
    // Kept fractional: `font-size: 10.5px` is a size a designer chose, and
    // rounding it up measures every word in that label three percent wide.
    if (l.unit == Length::Unit::Px) s.font_px = l.value + l.offset_px;
    else if (l.unit == Length::Unit::Percent) s.font_px = l.value * (float)em / 100.0f;
    // A page that asks for 400px type gets type, not a screen full of one word.
    if (s.font_px < 8) s.font_px = 8;
    if (s.font_px > 64) s.font_px = 64;
    return;
  }
  if (p == "font-weight") {
    if (lv == "bold" || lv == "bolder" || lv == "600" || lv == "700" || lv == "800" || lv == "900")
      s.bold = true;
    else if (lv == "normal" || lv == "lighter" || lv == "100" || lv == "200" || lv == "300" || lv == "400" ||
             lv == "500")
      s.bold = false;
    return;
  }
  if (p == "font-style") {
    s.italic = (lv == "italic" || lv == "oblique");
    return;
  }
  if (p == "font-family") {
    apply_family_list(s, lv);
    return;
  }
  if (p == "font") {
    for (const auto& part : split_parts(v)) {
      std::string lp = lowered(part);
      if (lp == "bold" || lp == "600" || lp == "700" || lp == "800" || lp == "900" || lp == "bolder") s.bold = true;
      if (lp == "normal" || lp == "400") s.bold = false;
      if (lp == "italic" || lp == "oblique") s.italic = true;
      size_t slash = lp.find('/');
      std::string size_part = slash == std::string::npos ? lp : lp.substr(0, slash);
      Length l = parse_length(size_part, em, root_px);
      if (l.unit == Length::Unit::Px && l.value >= 4 && l.value <= 128) {
        s.font_px = l.value + l.offset_px;
        if (slash != std::string::npos) {
          std::string lh_part = lp.substr(slash + 1);
          size_t k = 0;
          float n = 0;
          if (parse_number(lh_part, k, n)) {
            std::string unit = trimmed(lh_part.substr(k));
            if (unit.empty()) {
              s.line_height_factor = n;
              s.line_height = round_to_int(n * s.font_px);
            } else {
              Length lh = parse_length(lh_part, round_to_int(s.font_px), root_px);
              if (lh.unit == Length::Unit::Px) s.line_height = round_to_int(lh.value + lh.offset_px);
            }
          }
        }
      }
      if (lp.find("monospace") != std::string::npos || lp.find("courier") != std::string::npos ||
          lp.find("consolas") != std::string::npos || lp.find("mono") != std::string::npos) s.mono = true;
    }
    // The family list is everything after the size token; scanning the whole
    // value for a known family name is enough because sizes never spell one.
    apply_family_list(s, lv);
    return;
  }
  if (p == "text-decoration" || p == "text-decoration-line") {
    if (lv.find("underline") != std::string::npos) s.underline = true;
    if (lv.find("line-through") != std::string::npos) s.strike = true;
    if (lv.find("none") != std::string::npos) { s.underline = false; s.strike = false; }
    return;
  }
  if (p == "text-align") {
    if (lv == "center") s.text_align = Align::Center;
    else if (lv == "right" || lv == "end") s.text_align = Align::Right;
    else if (lv == "justify") s.text_align = Align::Justify;
    else s.text_align = Align::Left;
    return;
  }
  if (p == "white-space") {
    if (lv == "pre" || lv == "pre-wrap" || lv == "break-spaces") s.white_space = WhiteSpace::Pre;
    else if (lv == "nowrap" || lv == "pre-line") s.white_space = WhiteSpace::Nowrap;
    else s.white_space = WhiteSpace::Normal;
    return;
  }
  if (p == "line-height") {
    // `normal`: the face's own height, and nothing inherited -- 0 says so.
    if (lv == "normal") { s.line_height = 0; s.line_height_factor = 0.0f; return; }
    size_t k = 0;
    float n = 0;
    if (parse_number(lv, k, n) && trimmed(lv.substr(k)).empty()) {
      s.line_height_factor = n;
      s.line_height = round_to_int(n * (float)(s.font_px > 0 ? s.font_px : em));
      return;
    }
    Length l = parse_length(v, s.font_px > 0 ? s.font_px : em, root_px);
    if (l.unit == Length::Unit::Px) { s.line_height = round_to_int(l.value); s.line_height_factor = 0.0f; }
    else if (l.unit == Length::Unit::Percent) {
      s.line_height = round_to_int(l.value * (float)(s.font_px > 0 ? s.font_px : em) / 100.0f);
      s.line_height_factor = 0.0f;
    }
    return;
  }
  if (p == "margin") { apply_box_shorthand(v, em, root_px, s.margin, s.margin_auto); return; }
  if (p == "padding") { apply_box_shorthand(v, em, root_px, s.padding); return; }
  // `grid-template: <rows> / <columns>` -- the shorthand a page-frame grid is
  // written in (`min-content 1fr min-content / 12.25rem minmax(0,1fr)`). The
  // slash separates the two lists; with no slash there is nothing to set but
  // the rows. Without this the whole shorthand was dropped, the grid fell back
  // to equal columns, and a sidebar the size of the page took half of it.
  if (p == "grid-template") {
    int depth = 0;
    size_t slash = std::string::npos;
    for (size_t i = 0; i < lv.size(); ++i) {
      if (lv[i] == '(') ++depth;
      else if (lv[i] == ')' && depth > 0) --depth;
      else if (lv[i] == '/' && depth == 0) { slash = i; break; }
    }
    if (lv == "none") { s.grid_template.clear(); s.grid_template_rows.clear(); return; }
    if (slash == std::string::npos) { s.grid_template_rows = trimmed(lv); return; }
    s.grid_template_rows = trimmed(lv.substr(0, slash));
    s.grid_template = trimmed(lv.substr(slash + 1));
    return;
  }
  if (p == "grid-template-columns") { s.grid_template = lv; return; }
  if (p == "grid-template-rows") { s.grid_template_rows = lv; return; }
  if (p == "grid-template-areas") { s.grid_template_areas = v; return; }
  // Area names in `grid-template-areas` keep the author's case ('pageContent'
  // stays 'pageContent'); `lv` is lowercased for keyword matching everywhere
  // else, and using it here made a mixed-case area name (Wikipedia's own
  // vector skin: `.mw-content-container{grid-area:pageContent}` against
  // `grid-template-areas:'columnStart pageContent'`) fail to look up in
  // area_map -- silently falling through to auto-placement in the WRONG
  // column and squeezing the whole article body into 15px.
  if (p == "grid-area") { s.grid_area = v; return; }
  if (p == "grid-column") { s.grid_column = lv; return; }
  if (p == "grid-row") { s.grid_row = lv; return; }
  if (p == "flex-wrap") {
    s.flex_wrap = lv.find("wrap") != std::string::npos && lv.find("nowrap") == std::string::npos;
    s.wrap_reverse = lv.find("wrap-reverse") != std::string::npos;
    return;
  }
  if (p == "flex-flow") {
    s.flex_dir = (lv.find("column") != std::string::npos) ? FlexDir::Column : FlexDir::Row;
    s.flex_reverse = lv.find("reverse") != std::string::npos && lv.find("wrap-reverse") == std::string::npos
                     ? true : lv.find("row-reverse") != std::string::npos || lv.find("column-reverse") != std::string::npos;
    s.wrap_reverse = lv.find("wrap-reverse") != std::string::npos;
    s.flex_wrap = lv.find("wrap") != std::string::npos && lv.find("nowrap") == std::string::npos;
    return;
  }
  if (p == "min-width") { s.min_width = parse_length(v, em, root_px); return; }
  if (p == "max-width") { s.max_width = parse_length(v, em, root_px); return; }
  if (p == "gap" || p == "grid-gap") {
    // `gap: row column` or one value for both.
    std::vector<std::string> parts;
    std::string cur;
    for (char c : lv) { if (c == ' ' || c == '\t') { if (!cur.empty()) { parts.push_back(cur); cur.clear(); } } else cur += c; }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.empty()) return;
    s.row_gap = px_or(parse_length(parts[0], em, root_px), 0);
    s.col_gap = parts.size() > 1 ? px_or(parse_length(parts[1], em, root_px), 0) : s.row_gap;
    return;
  }
  if (p == "column-gap" || p == "grid-column-gap") { s.col_gap = px_or(parse_length(v, em, root_px), 0); return; }
  if (p == "row-gap" || p == "grid-row-gap") { s.row_gap = px_or(parse_length(v, em, root_px), 0); return; }
  if (p == "margin-top") { Length l = parse_length(v, em, root_px); s.margin[0] = px_or(l, 0); s.margin_auto[0] = is_auto_keyword(v); return; }
  if (p == "margin-right") { Length l = parse_length(v, em, root_px); s.margin[1] = px_or(l, 0); s.margin_auto[1] = is_auto_keyword(v); return; }
  if (p == "margin-bottom") { Length l = parse_length(v, em, root_px); s.margin[2] = px_or(l, 0); s.margin_auto[2] = is_auto_keyword(v); return; }
  if (p == "margin-left") { Length l = parse_length(v, em, root_px); s.margin[3] = px_or(l, 0); s.margin_auto[3] = is_auto_keyword(v); return; }
  if (p == "padding-top") { s.padding[0] = px_or(parse_length(v, em, root_px), 0); return; }
  if (p == "padding-right") { s.padding[1] = px_or(parse_length(v, em, root_px), 0); return; }
  if (p == "padding-bottom") { s.padding[2] = px_or(parse_length(v, em, root_px), 0); return; }
  if (p == "padding-left") { s.padding[3] = px_or(parse_length(v, em, root_px), 0); return; }
  if (p == "content") {
    const std::string t = trimmed(v);
    const std::string low2 = lowered(t);
    if (low2 == "none" || low2 == "normal") { s.has_content = false; return; }
    // Quoted strings, joined; `attr(name)` reads the element's attribute. A
    // counter() or an image is not followed -- what comes out is the empty
    // string, which is still a box, which is what the page asked for.
    std::string out;
    size_t i = 0;
    while (i < t.size()) {
      while (i < t.size() && is_space(t[i])) ++i;
      if (i >= t.size()) break;
      if (t[i] == '"' || t[i] == '\'') {
        const char q = t[i++];
        while (i < t.size() && t[i] != q) {
          if (t[i] == '\\' && i + 1 < t.size()) {
            // \2014 and friends: a CSS escape for a code point.
            size_t j = i + 1;
            std::string hex;
            while (j < t.size() && hex.size() < 6 && isxdigit((unsigned char)t[j])) hex += t[j++];
            if (!hex.empty()) {
              unsigned cp = (unsigned)strtoul(hex.c_str(), nullptr, 16);
              // UTF-8, so the renderer sees the same bytes a page would send.
              if (cp < 0x80) out += (char)cp;
              else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
              else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
              i = j;
              if (i < t.size() && is_space(t[i])) ++i;
              continue;
            }
            out += t[++i];
            ++i;
            continue;
          }
          out += t[i++];
        }
        ++i;
        s.has_content = true;
        continue;
      }
      if (low2.compare(i, 5, "attr(") == 0) {
        const size_t close = t.find(')', i);
        if (close == std::string::npos) break;
        // In PLACE, not appended: `content: attr(x) ": "` reads "Host: ", and
        // resolving it at the end produced ": Host". The attribute name is
        // parked between two bytes no page's text contains, and substituted
        // when the element is in hand.
        out += '\x01';
        out += trimmed(t.substr(i + 5, close - i - 5));
        out += '\x02';
        s.has_content = true;
        i = close + 1;
        continue;
      }
      // An unrecognised term (counter, url, open-quote): skip its token.
      while (i < t.size() && !is_space(t[i])) ++i;
      s.has_content = true;
    }
    s.content = out;
    return;
  }
  if (p == "rotate") {
    float a = 0;
    s.rotate_rad = parse_angle(lowered(trimmed(v)), a) ? a : 0.0f;
    return;
  }
  if (p == "scale") {
    const auto parts = split_parts(lowered(trimmed(v)));
    s.scale_x = s.scale_y = 1.0f;
    if (!parts.empty() && parts[0] != "none") {
      s.scale_x = s.scale_y = scale_factor(parts[0]);
      if (parts.size() > 1) s.scale_y = scale_factor(parts[1]);
    }
    return;
  }
  if (p == "transform-origin") {
    // Up to two of: a length, a percentage, left/center/right, top/center/bottom. One vertical keyword alone is y.
    const auto parts = split_parts(lowered(trimmed(v)));
    Length half; half.unit = Length::Unit::Percent; half.value = 50;
    s.origin_x = s.origin_y = half;
    auto keyword = [](const std::string& t, Length& out, bool& vertical) {
      vertical = t == "top" || t == "bottom";
      if (t == "left" || t == "top") { out.unit = Length::Unit::Percent; out.value = 0; return true; }
      if (t == "center") { out.unit = Length::Unit::Percent; out.value = 50; return true; }
      if (t == "right" || t == "bottom") { out.unit = Length::Unit::Percent; out.value = 100; return true; }
      return false;
    };
    bool first_vertical = false;
    for (size_t k = 0; k < parts.size() && k < 2; ++k) {
      Length l;
      bool vertical = false;
      if (!keyword(parts[k], l, vertical)) l = parse_length(parts[k], s.font_px > 0 ? s.font_px : em, root_px);
      if (k == 0) {
        first_vertical = vertical;
        if (vertical) s.origin_y = l; else s.origin_x = l;
      } else {
        if (first_vertical) s.origin_x = l; else s.origin_y = l;
      }
    }
    return;
  }
  if (p == "transform" || p == "translate") {
    const std::string lv2 = lowered(trimmed(v));
    if (lv2 == "none" || lv2.empty()) {
      s.has_translate = false;
      if (p == "transform") s.transform_fns.clear();
      return;
    }
    if (p == "transform") {
      // The whole list, in order. If it is translations only it is left to the path below.
      std::vector<Style::TransformFn> fns;
      bool beyond_translate = false;
      size_t i = 0;
      while (i < lv2.size()) {
        const size_t open = lv2.find('(', i);
        if (open == std::string::npos) break;
        const size_t close = lv2.find(')', open);
        if (close == std::string::npos) break;
        const std::string name = trimmed(lv2.substr(i, open - i));
        std::vector<std::string> args;
        {
          std::string a = lv2.substr(open + 1, close - open - 1);
          for (auto& ch : a) if (ch == ',') ch = ' ';
          args = split_parts(a);
        }
        i = close + 1;
        Style::TransformFn f;
        auto num = [&](size_t k, float dflt) {
          if (k >= args.size()) return dflt;
          size_t at = 0;
          float n = dflt;
          return parse_number(args[k], at, n) ? n : dflt;
        };
        auto ang = [&](size_t k) {
          float a = 0;
          return k < args.size() && parse_angle(args[k], a) ? a : 0.0f;
        };
        const int fpx = s.font_px > 0 ? s.font_px : em;
        if (name == "translate" || name == "translatex" || name == "translatey" || name == "translate3d") {
          f.kind = Style::TransformFn::Kind::Translate;
          if (name == "translatey") f.ty = args.empty() ? Length{} : parse_length(args[0], fpx, root_px);
          else {
            if (!args.empty()) f.tx = parse_length(args[0], fpx, root_px);
            if (name != "translatex" && args.size() > 1) f.ty = parse_length(args[1], fpx, root_px);
          }
        } else if (name == "scale" || name == "scalex" || name == "scaley" || name == "scale3d") {
          f.kind = Style::TransformFn::Kind::Scale;
          const float a = args.empty() ? 1.0f : scale_factor(args[0]);
          const float b = args.size() > 1 ? scale_factor(args[1]) : a;
          f.v[0] = name == "scaley" ? 1.0f : a;
          f.v[1] = name == "scalex" ? 1.0f : name == "scaley" ? a : b;
          beyond_translate = true;
        } else if (name == "rotate" || name == "rotatez") {
          f.kind = Style::TransformFn::Kind::Rotate;
          f.v[0] = ang(0);
          beyond_translate = true;
        } else if (name == "rotatex" || name == "rotatey") {
          // A turn about an axis in the page, with no perspective: what is left on screen is a squash.
          f.kind = Style::TransformFn::Kind::Scale;
          const float c = std::cos(ang(0));
          f.v[0] = name == "rotatey" ? c : 1.0f;
          f.v[1] = name == "rotatex" ? c : 1.0f;
          beyond_translate = true;
        } else if (name == "skew" || name == "skewx" || name == "skewy") {
          f.kind = Style::TransformFn::Kind::Skew;
          f.v[0] = name == "skewy" ? 0.0f : ang(0);
          f.v[1] = name == "skewy" ? ang(0) : name == "skew" ? ang(1) : 0.0f;
          beyond_translate = true;
        } else if (name == "matrix" && args.size() == 6) {
          f.kind = Style::TransformFn::Kind::Matrix;
          for (int k = 0; k < 6; ++k) f.v[k] = num((size_t)k, 0);
          beyond_translate = true;
        } else if (name == "matrix3d" && args.size() == 16) {
          f.kind = Style::TransformFn::Kind::Matrix;   // the part of it that lands on the page plane
          f.v[0] = num(0, 1); f.v[1] = num(1, 0); f.v[2] = num(4, 0); f.v[3] = num(5, 1); f.v[4] = num(12, 0); f.v[5] = num(13, 0);
          beyond_translate = true;
        } else {
          continue;   // perspective(), rotate3d(), ...: nothing a flat page shows
        }
        fns.push_back(f);
      }
      if (beyond_translate) {
        s.transform_fns = std::move(fns);
        s.has_translate = false;
        return;
      }
      s.transform_fns.clear();
    }
    // `translate(x[, y])`, `translateX(x)`, `translateY(y)`, and the standalone
    // `translate: x y` property. Anything else in the list is left alone.
    auto take = [&](const std::string& fn, int which) {
      const size_t at = lv2.find(fn + "(");
      if (at == std::string::npos) return;
      const size_t open = lv2.find('(', at);
      const size_t close = lv2.find(')', open);
      if (close == std::string::npos) return;
      const std::string args = lv2.substr(open + 1, close - open - 1);
      const size_t comma = args.find(',');
      const std::string a1 = trimmed(comma == std::string::npos ? args : args.substr(0, comma));
      const std::string a2 = comma == std::string::npos ? std::string() : trimmed(args.substr(comma + 1));
      if (which == 0 || which == 2) { s.translate_x = parse_length(a1, em, root_px); s.has_translate = true; }
      if (which == 1) { s.translate_y = parse_length(a1, em, root_px); s.has_translate = true; }
      if (which == 2 && !a2.empty()) { s.translate_y = parse_length(a2, em, root_px); }
    };
    take("translatex", 0);
    take("translatey", 1);
    take("translate3d", 2);
    if (lv2.find("translate(") != std::string::npos) take("translate", 2);
    if (p == "translate" && lv2.find('(') == std::string::npos) {
      // The standalone property: `translate: 10px 4px`.
      const auto parts = split_parts(lv2);
      if (!parts.empty()) { s.translate_x = parse_length(parts[0], em, root_px); s.has_translate = true; }
      if (parts.size() > 1) s.translate_y = parse_length(parts[1], em, root_px);
    }
    return;
  }
  if (p == "box-sizing") {
    s.border_box = lowered(trimmed(v)) == "border-box";
    return;
  }
  if (p == "box-shadow") {
    const std::string low = lowered(trimmed(v));
    if (low == "none" || low.empty()) { s.has_shadow = false; return; }
    // The first shadow in the list, and only an outer one: an inset shadow is
    // a different painting job and drawing it outside the box is worse than
    // not drawing it.
    //
    // Split on the first TOP-LEVEL comma. Splitting on the first comma of any
    // kind cuts `rgba(0,0,0,.45)` in half, and `rgba(0` parses as opaque black
    // -- which is a shadow four times too dark, painted as a hard black slab
    // under every card.
    size_t cut = v.size();
    {
      int depth = 0;
      for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '(') ++depth;
        else if (v[i] == ')') { if (depth) --depth; }
        else if (v[i] == ',' && depth == 0) { cut = i; break; }
      }
    }
    std::string first = trimmed(v.substr(0, cut));
    if (lowered(first).find("inset") != std::string::npos) { s.has_shadow = false; return; }
    int lens[4] = {0, 0, 0, 0};
    int n = 0;
    bool have_colour = false;
    uint32_t col = 0;
    for (const auto& part : split_parts(first)) {
      uint32_t c = 0;
      if (!have_colour && parse_colour(part, c)) { col = c; have_colour = true; continue; }
      if (n < 4) lens[n++] = px_or(parse_length(part, em, root_px), 0);
    }
    if (n < 2) { s.has_shadow = false; return; }
    s.has_shadow = true;
    s.shadow_x = lens[0];
    s.shadow_y = lens[1];
    s.shadow_blur = lens[2] < 0 ? 0 : lens[2];
    s.shadow_spread = lens[3];
    s.shadow_color = have_colour ? col : 0x40000000u;
    return;
  }
  if (p == "letter-spacing") {
    const std::string t = lowered(trimmed(v));
    if (t == "normal") { s.letter_spacing = 0; return; }
    const Length len = parse_length(v, em, root_px);
    // Kept fractional. The pen advances in floats and only the final position
    // is rounded, so a sub-pixel track accumulates the way the type designer
    // meant rather than being multiplied up to a whole pixel per glyph.
    s.letter_spacing = len.unit == Length::Unit::Px ? (len.value + len.offset_px) : 0.0f;
    return;
  }
  if (p == "opacity") {
    // A number, or a percentage. `opacity: 0` is how a page keeps a control in
    // the layout and out of sight until it is hovered -- sixteen rules in the
    // Assay console alone -- and without this every one of them was painted.
    std::string t = trimmed(v);
    float f = (float)std::atof(t.c_str());
    if (!t.empty() && t.back() == '%') f /= 100.0f;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    s.opacity = f;
    return;
  }
  if (p == "border-radius" || p == "border-top-left-radius" || p == "border-top-right-radius" ||
      p == "border-bottom-left-radius" || p == "border-bottom-right-radius") {
    // One radius, from the largest corner named. `50%` on a square box is a
    // circle, which is how every avatar and pill on the web is drawn -- resolve
    // a percentage against the box later, where the box is known, by storing it
    // as a large radius the painter clamps to half the shorter side.
    int biggest = 0;
    for (const auto& part : split_parts(v)) {
      if (part.find('/') != std::string::npos) continue;   // elliptical: not followed
      if (part.find('%') != std::string::npos) { biggest = 9999; continue; }
      const int px = px_or(parse_length(part, em, root_px), 0);
      if (px > biggest) biggest = px;
    }
    if (biggest > 0) s.radius = biggest;
    return;
  }
  if (p == "border" || p == "border-top" || p == "border-right" || p == "border-bottom" || p == "border-left" ||
      p == "border-width" || p == "border-color" || p == "border-style") {
    int width = -1;
    bool none = false;
    uint32_t c = 0;
    bool have_colour = false;
    for (const auto& part : split_parts(v)) {
      std::string lp = lowered(part);
      if (lp == "none" || lp == "hidden") { none = true; continue; }
      if (lp == "solid" || lp == "dashed" || lp == "dotted" || lp == "double" || lp == "groove" ||
          lp == "ridge" || lp == "inset" || lp == "outset")
        continue;
      if (lp == "thin") { width = 1; continue; }
      if (lp == "medium") { width = 2; continue; }
      if (lp == "thick") { width = 4; continue; }
      Length l = parse_length(part, em, root_px);
      if (l.unit == Length::Unit::Px) { width = round_to_int(l.value); continue; }
      if (parse_colour(part, c)) { have_colour = true; continue; }
    }
    if (p == "border-color") {
      if (have_colour) s.border_color = c;
      return;
    }
    if (have_colour) s.border_color = c;
    int w = none ? 0 : (width < 0 ? (p == "border-style" ? 1 : 0) : width);
    if (p == "border" || p == "border-width" || p == "border-style") {
      s.border[0] = s.border[1] = s.border[2] = s.border[3] = w;
    } else if (p == "border-top") s.border[0] = w;
    else if (p == "border-right") s.border[1] = w;
    else if (p == "border-bottom") s.border[2] = w;
    else if (p == "border-left") s.border[3] = w;
    return;
  }
  if (p == "width") {
    s.width = parse_length(v, em, root_px);
    return;
  }
  if (p == "height") {
    s.height = parse_length(v, em, root_px);
    return;
  }
  if (p == "list-style" || p == "list-style-type") {
    if (lv.find("none") != std::string::npos) s.list_bullet = false;
    else s.list_bullet = true;
    return;
  }
  if (p == "text-transform") {
    const std::string a = lowered(trimmed(v));
    if (a == "uppercase") s.text_case = Style::TextCase::Upper;
    else if (a == "lowercase") s.text_case = Style::TextCase::Lower;
    else if (a == "capitalize") s.text_case = Style::TextCase::Capitalize;
    else s.text_case = Style::TextCase::None;
    return;
  }
}

// The attributes that are styling in disguise, turned into declarations.
//
// Half the web predates CSS. `<table border=1 cellpadding=4 bgcolor=#eee>`,
// `<td align=center width=120>`, `<font color=red size=4>` and `<body
// bgcolor=black text=white>` are how those pages say what they look like, and
// an engine that ignores them renders 1997 as undifferentiated grey. The spec
// calls these presentational hints and puts them BELOW author CSS and ABOVE
// the UA sheet, which is where the cascade below places them.
std::vector<Declaration> presentational_hints(const Node& el) {
  std::vector<Declaration> out;
  auto add = [&](const char* prop, const std::string& value) {
    if (!value.empty()) out.push_back({prop, value, false});
  };
  auto is_number = [](const std::string& v) {
    if (v.empty()) return false;
    for (char c : v)
      if (c < '0' || c > '9') return false;
    return true;
  };
  // A length attribute is pixels unless it ends in a percent sign.
  auto length_attr = [&](const std::string& v) -> std::string {
    std::string t = trimmed(v);
    if (t.empty()) return {};
    if (t.back() == '%') return t;
    std::string digits;
    for (char c : t) {
      if (c < '0' || c > '9') break;
      digits += c;
    }
    return digits.empty() ? std::string() : digits + "px";
  };

  const std::string& tag = el.tag;

  if (tag == "body") {
    add("background-color", el.attr("bgcolor"));
    add("color", el.attr("text"));
  }
  if (tag == "font" || tag == "basefont") {
    add("color", el.attr("color"));
    add("font-family", el.attr("face"));
    // `size` is a 1..7 scale, or relative +/- delta.
    std::string sz = trimmed(el.attr("size"));
    if (!sz.empty()) {
      static const char* const kFontSizes[8] = {"15px", "10px", "12px", "15px",
                                                "18px", "24px", "32px", "48px"};
      if (sz[0] == '+' || sz[0] == '-') {
        int delta = 0;
        try { delta = std::stoi(sz); } catch (...) {}
        int n = std::max(1, std::min(7, 3 + delta));
        add("font-size", kFontSizes[n]);
      } else if (is_number(sz)) {
        int n = sz[0] - '0';
        if (n >= 1 && n <= 7) add("font-size", kFontSizes[n]);
      }
    }
  }
  if (tag == "table" || tag == "td" || tag == "th" || tag == "tr" || tag == "thead" || tag == "tbody") {
    add("background-color", el.attr("bgcolor"));
  }
  if (tag == "table") {
    std::string b = trimmed(el.attr("border"));
    if (!b.empty() && b != "0") add("border", length_attr(b) + " solid #4a4a4a");
    add("width", length_attr(el.attr("width")));
  }
  if (tag == "tr") {
    add("height", length_attr(el.attr("height")));
  }
  if (tag == "td" || tag == "th") {
    // The border a cell gets comes from its TABLE's border attribute, so the
    // cell has to look for the table it is in.
    const Node* up = el.parent;
    for (int hops = 0; up && hops < 8; ++hops, up = up->parent) {
      if (up->kind != Node::Kind::Element || up->tag != "table") continue;
      std::string b = trimmed(up->attr("border"));
      if (!b.empty() && b != "0") out.push_back({"border", "1px solid #4a4a4a", false});
      std::string pad = trimmed(up->attr("cellpadding"));
      if (!pad.empty() && is_number(pad)) out.push_back({"padding", pad + "px", false});
      break;
    }
    add("width", length_attr(el.attr("width")));
    add("height", length_attr(el.attr("height")));
    // `<td nowrap>` is the pre-CSS way to say "this cell is one line", and
    // pages that lay out with tables lean on it. Without it a narrow column
    // broke "Advanced search" across two lines and clipped both.
    if (el.has_attr("nowrap")) out.push_back({"white-space", "nowrap", false});
  }
  if (tag == "td" || tag == "th" || tag == "tr" || tag == "table" || tag == "div" || tag == "p" ||
      tag == "caption" || tag == "center" || tag == "col" || tag == "colgroup") {
    std::string al = lowered(trimmed(el.attr("align")));
    if (al == "center" || al == "left" || al == "right") add("text-align", al);
    if (tag == "center") add("text-align", "center");
  }
  if (tag == "center") add("text-align", "center");
  if (tag == "hr") {
    std::string sz = length_attr(el.attr("size"));
    if (!sz.empty()) add("border-top", sz + " solid #333333");
    add("width", length_attr(el.attr("width")));
  }
  if (tag == "img") {
    add("width", length_attr(el.attr("width")));
    add("height", length_attr(el.attr("height")));
  }
  if (tag == "ul" || tag == "ol" || tag == "li") {
    if (lowered(trimmed(el.attr("type"))) == "none") add("list-style", "none");
  }
  return out;
}

}  // namespace
struct Hit {
  const Declaration* decl;
  // 0 the UA sheet, 1 the HTML presentational attributes, 2 the author's
  // sheets, 3 the element's own style="". The spec's order, and the reason
  // `<table border=1>` styles a table but a page's own CSS still wins.
  int origin;
  int specificity;
  int order;
};
struct HitCache {
  Style computed;
  bool style_valid = false;
  Style before_style;
  Style after_style;
  bool checked_before = false;
  bool checked_after = false;
  bool has_before = false;
  bool has_after = false;
  std::vector<Hit> hits;
  std::shared_ptr<std::vector<Declaration>> hints;
  std::shared_ptr<Stylesheet> inl;
  const Stylesheet* ua = nullptr; const Stylesheet* author = nullptr; int vw = 0;
};
static std::unordered_map<const Node*, HitCache> g_hit_cache;
// Bloom of a node and all its ancestors, memoized for the layout (cleared with
// the hit cache). Built iteratively: a hostile document may nest very deep.
extern bool g_style_cache_on;
static std::unordered_map<const Node*, AncBloom> g_bloom_cache;
const AncBloom* bloom_of_ancestors(const Node& el) {
  if (!g_style_cache_on || !el.parent) return nullptr;
  std::vector<const Node*> chain;
  const Node* p = el.parent;
  while (p && !g_bloom_cache.count(p)) { chain.push_back(p); p = p->parent; }
  AncBloom cur;
  if (p) cur = g_bloom_cache[p];
  for (size_t k = chain.size(); k-- > 0;) {
    bloom_add_node(cur, *chain[k]);
    g_bloom_cache[chain[k]] = cur;
  }
  return &g_bloom_cache[el.parent];
}
double g_cs_ms = 0, g_cs_match = 0; long g_cs_n = 0, g_cs_miss = 0;
bool g_style_cache_on = false;
void clear_style_cache() { g_hit_cache.clear(); g_bloom_cache.clear(); }

bool pseudo_content(const Node& element, const Style& parent, const Stylesheet& ua,
                    const Stylesheet& author, int viewport_w, bool after, Style* out) {
  if (!out) return false;
  if (g_style_cache_on) {
    auto cit = g_hit_cache.find(&element);
    if (cit != g_hit_cache.end() && cit->second.ua == &ua && cit->second.author == &author && cit->second.vw == viewport_w) {
      if (after && cit->second.checked_after) {
        if (!cit->second.has_after) return false;
        *out = cit->second.after_style;
        return true;
      }
      if (!after && cit->second.checked_before) {
        if (!cit->second.has_before) return false;
        *out = cit->second.before_style;
        return true;
      }
    }
  }
  const SimpleSelector::PseudoEl want =
      after ? SimpleSelector::PseudoEl::After : SimpleSelector::PseudoEl::Before;

  std::vector<Hit> hits;
  int order = 0;
  bool any = false;
  std::vector<uint32_t> candidates;
  auto collect = [&](const Stylesheet& sheet, int origin) {
    candidate_rules_pseudo(sheet, element, after, candidates);
    if (candidates.empty()) return;
    struct BloomScope { BloomScope(const Node& e) { g_bloom_el = &e; g_bloom = bloom_of_ancestors(e); } ~BloomScope() { g_bloom = nullptr; g_bloom_el = nullptr; } } bloom_scope_(element);
    for (const uint32_t ri : candidates) {
      const auto& rule = sheet.rules[ri];
      if (!rule_media_ok(rule, viewport_w)) continue;
      for (const auto& sel : rule.selectors) {
        if (sel.parts.empty()) continue;
        if (sel.parts.back().pseudo != want) continue;
        // A compound like `a:has(...)::after` carries BOTH markers: `never`
        // was set once for being a pseudo-element and again, independently,
        // for the `:has()` this engine cannot evaluate. Clearing it
        // unconditionally threw the `:has()` condition away too, so
        // `a:has(...)::after{content:']'}` matched every plain `<a>` on the
        // page -- proven on Wikipedia's own VisualEditor-fallback rule, which
        // stamped a stray "]" after every wikilink once its `.ve-not-available`
        // ancestor class landed (added by script, not present in the raw
        // HTML). If the reason is unevaluable, stay conservative and skip
        // the rule rather than guess it matches.
        if (sel.parts.back().never_unevaluable) continue;
        // The compound carrying the pseudo matches nothing on purpose. Clear
        // that for this test only: what has to match is the ELEMENT the
        // generated box hangs off.
        // Copied only when there is something to clear: copying every
        // candidate selector for every element was a malloc storm.
        if (sel.parts.back().never || sel.parts.back().pseudo != SimpleSelector::PseudoEl::None) {
          Selector probe = sel;
          probe.parts.back().never = false;
          probe.parts.back().pseudo = SimpleSelector::PseudoEl::None;
          if (!matches(element, probe)) continue;
        } else if (!matches(element, sel)) {
          continue;
        }
        any = true;
        for (const auto& d : rule.decls)
          hits.push_back({&d, origin, sel.specificity(), order++});
      }
    }
  };
  collect(ua, 0);
  collect(author, 2);
  if (!any) {
    if (g_style_cache_on) {
      auto cit = g_hit_cache.find(&element);
      if (cit != g_hit_cache.end()) {
        if (after) { cit->second.checked_after = true; cit->second.has_after = false; }
        else { cit->second.checked_before = true; cit->second.has_before = false; }
      }
    }
    return false;
  }

  std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
    if (a.decl->important != b.decl->important) return !a.decl->important;
    if (a.origin != b.origin) return a.origin < b.origin;
    if (a.specificity != b.specificity) return a.specificity < b.specificity;
    return a.order < b.order;
  });

  Style s;
  inherit(s, parent);
  s.display = Display::Inline;
  s.has_background = false;
  s.opacity = 1.0f;
  s.effective_opacity = parent.effective_opacity;
  for (const auto& h : hits) apply_declaration(s, *h.decl, parent, viewport_w);
  if (!s.has_content) {
    if (g_style_cache_on) {
      auto cit = g_hit_cache.find(&element);
      if (cit != g_hit_cache.end()) {
        if (after) { cit->second.checked_after = true; cit->second.has_after = false; }
        else { cit->second.checked_before = true; cit->second.has_before = false; }
      }
    }
    return false;
  }
  // Substitute any attr() placeholders now that the element is known.
  if (s.content.find('\x01') != std::string::npos) {
    std::string built;
    for (size_t i = 0; i < s.content.size(); ++i) {
      if (s.content[i] != '\x01') { built += s.content[i]; continue; }
      const size_t end = s.content.find('\x02', i);
      if (end == std::string::npos) break;
      built += element.attr(s.content.substr(i + 1, end - i - 1));
      i = end;
    }
    s.content = built;
  }
  s.effective_opacity = parent.effective_opacity * s.opacity;
  *out = s;
  if (g_style_cache_on) {
    auto cit = g_hit_cache.find(&element);
    if (cit != g_hit_cache.end()) {
      if (after) { cit->second.checked_after = true; cit->second.has_after = true; cit->second.after_style = s; }
      else { cit->second.checked_before = true; cit->second.has_before = true; cit->second.before_style = s; }
    }
  }
  return true;
}

static Style compute_style_inner(const Node& element, const Style& parent, const Stylesheet& ua, const Stylesheet& author,
                    int viewport_w) {
  std::unordered_map<const Node*, HitCache>::iterator cit;
  bool cached = false;
  if (g_style_cache_on) {
    cit = g_hit_cache.find(&element);
    cached = cit != g_hit_cache.end() && cit->second.ua == &ua && cit->second.author == &author && cit->second.vw == viewport_w;
    if (cached && cit->second.style_valid) {
      return cit->second.computed;
    }
  }
  Style s;
  inherit(s, parent);
  s.display = Display::Inline;
  s.has_background = false;
  s.hidden = false;
  s.opacity = 1.0f;                 // not inherited: each box states its own
  s.effective_opacity = parent.effective_opacity;
  auto tm0 = std::chrono::steady_clock::now();
  if (!cached) {
  // Everything that matched, with what it is worth. Two sheets, so origin is
  // the outer key: an author rule of specificity 0 still beats the UA sheet.
  HitCache hc;
  std::vector<Hit>& hits = hc.hits;
  int order = 0;
  std::vector<uint32_t> candidates;
  auto collect = [&](const Stylesheet& sheet, int origin) {
    candidate_rules(sheet, element, candidates);
    struct BloomScope { BloomScope(const Node& e) { g_bloom_el = &e; g_bloom = bloom_of_ancestors(e); } ~BloomScope() { g_bloom = nullptr; g_bloom_el = nullptr; } } bloom_scope_(element);
    for (const uint32_t ri : candidates) {
      const auto& rule = sheet.rules[ri];
      if (!rule_media_ok(rule, viewport_w)) continue;
      int best = -1;
      for (const auto& sel : rule.selectors) {
        if (matches(element, sel)) {
          int spec = sel.specificity();
          if (spec > best) best = spec;
        }
      }
      if (best < 0) {
        order += (int)rule.decls.size();
        continue;
      }
#if !defined(AIOS_METAL)
      // AIOS_CSS_TRACE=<substring>: name every rule that matched an element
      // whose selector text or id/class contains that substring. The only way
      // to tell "the rule did not match" from "another rule won".
      if (const char* want = std::getenv("AIOS_CSS_TRACE")) {
        const std::string w(want);
        if (!w.empty() && (element.attr("class").find(w) != std::string::npos ||
                           element.attr("id").find(w) != std::string::npos)) {
          std::fputs(("[css] <" + element.tag + " class=\"" + element.attr("class") +
                      "\"> matched {" + rule.media + "} " + rule.source + " spec=" +
                      std::to_string(best) + ":").c_str(), stderr);
          for (const auto& d : rule.decls) std::fputs((" " + d.property + "=" + d.value + ";").c_str(), stderr);
          std::fputs("\n", stderr);
        }
      }
#endif
      for (const auto& d : rule.decls) hits.push_back({&d, origin, best, order++});
    }
  };
  collect(ua, 0);
  // The presentational attributes, as declarations. Kept alive for as long as
  // the hits reference them.
  auto hints = presentational_hints(element);
  if (!hints.empty()) {
    hc.hints = std::make_shared<std::vector<Declaration>>(std::move(hints));
    for (const auto& d : *hc.hints) hits.push_back({&d, 1, 0, order++});
  }
  collect(author, 2);

  // The element's own `style=""`, which outranks any selector.
  const std::string& inline_css = element.attr_ref("style");
  if (!inline_css.empty()) {
    hc.inl = std::make_shared<Stylesheet>(parse_css("* {" + inline_css + "}"));
    for (const auto& rule : hc.inl->rules)
      for (const auto& d : rule.decls) hits.push_back({&d, 3, 0, order++});
  }

  // Sort into the order they must be applied in. Insertion sort: the list is
  // short (one element's matched declarations), it is stable, and it does not
  // pull in a comparator-heavy sort into a freestanding build.
  for (size_t i = 1; i < hits.size(); ++i) {
    Hit h = hits[i];
    size_t j = i;
    auto weaker = [](const Hit& a, const Hit& b) {
      if (a.decl->important != b.decl->important) return !a.decl->important;
      if (a.origin != b.origin) return a.origin < b.origin;
      if (a.specificity != b.specificity) return a.specificity < b.specificity;
      return a.order < b.order;
    };
    while (j > 0 && weaker(h, hits[j - 1])) {
      hits[j] = hits[j - 1];
      --j;
    }
    hits[j] = h;
  }
  hc.ua = &ua; hc.author = &author; hc.vw = viewport_w;
    g_hit_cache[&element] = std::move(hc);
    cit = g_hit_cache.find(&element);
  }
  g_cs_match += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tm0).count(); if (!cached) ++g_cs_miss;
  const std::vector<Hit>& hits = cit->second.hits;
  struct Drop { bool on; const Node* n; ~Drop() { if (on) g_hit_cache.erase(n); } } drop_{!g_style_cache_on, &element};

  // Custom properties FIRST, in cascade order, because every other
  // declaration on this element may read one. An element that declares none
  // keeps sharing its parent's list; one that does gets its own copy with the
  // parent's entries beneath it, which is what makes them inherit.
  {
    bool own = false;
    std::shared_ptr<CustomProps> mine;
    for (const auto& h : hits) {
      const std::string& prop = h.decl->property;
      if (prop.size() < 3 || prop[0] != '-' || prop[1] != '-') continue;
      if (!own) {
        mine = s.custom ? std::make_shared<CustomProps>(*s.custom) : std::make_shared<CustomProps>();
        own = true;
      }
      // A later declaration of the same name replaces the earlier one rather
      // than shadowing it, so the list stays the size of the palette.
      const std::string value = substitute_vars(trimmed(h.decl->value), mine.get());
      bool replaced = false;
      for (auto& e : mine->entries)
        if (e.first == prop) { e.second = value; replaced = true; break; }
      if (!replaced) mine->entries.push_back({prop, value});
    }
    if (own) s.custom = mine;
  }

  for (const auto& h : hits) {
    const std::string& prop = h.decl->property;
    if (prop.size() >= 2 && prop[0] == '-' && prop[1] == '-') continue;   // already taken
    if (h.decl->value.find("var(") == std::string::npos) {
      apply_declaration(s, *h.decl, parent, viewport_w);
      continue;
    }
    Declaration resolved{prop, substitute_vars(h.decl->value, s.custom.get()), h.decl->important};
    // A var() that resolved to nothing leaves the property unset -- which is
    // what the spec calls invalid at computed-value time, and is much better
    // than parsing "" as a length of zero.
    if (!trimmed(resolved.value).empty()) apply_declaration(s, resolved, parent, viewport_w);
  }

  // `<a href>` is a link; `<a name>` is an anchor and gets none of the paint.
  if (element.tag == "a" && !element.has_attr("href")) {
    s.underline = false;
    s.color = parent.color;
  }
  if (s.font_px < 8) s.font_px = 8;
  // The number was inherited, not the length: this box's own size decides.
  if (s.line_height_factor > 0.0f) s.line_height = round_to_int(s.line_height_factor * (float)s.font_px);
  // Opacity multiplies down the tree: a dimmed panel dims what is inside it.
  s.effective_opacity = parent.effective_opacity * s.opacity;
  if (s.effective_opacity < 0.0f) s.effective_opacity = 0.0f;
  if (s.effective_opacity > 1.0f) s.effective_opacity = 1.0f;
  if (g_style_cache_on) {
    cit->second.computed = s;
    cit->second.style_valid = true;
  }
  return s;
}
Style compute_style(const Node& element, const Style& parent, const Stylesheet& ua, const Stylesheet& author,
                    int viewport_w) {
  auto t0 = std::chrono::steady_clock::now();
  Style r = compute_style_inner(element, parent, ua, author, viewport_w);
  g_cs_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); ++g_cs_n;
  return r;
}

}  // namespace aios::userland::web
