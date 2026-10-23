// Type: the font a request maps onto, how wide a run of text is, and drawing it
// into a buffer - what the web pane and faces both set text with. Moved out of
// web_pane.cpp unchanged, so text can be measured and drawn without a desktop
// or a browser: the glyphs come from the font atlas and the outline engine,
// the pixels go through userland/pixels.
#include "aios/userland/web_type.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "aios/userland/font_atlas.hpp"
#include "aios/userland/font_kerning.hpp"
#include "aios/userland/glyph_engine.hpp"
#include "aios/userland/pixels.hpp"

namespace aios::userland {

// The punctuation a real page is full of, mapped to what the ASCII atlas can
// draw. Without this a page's em dashes, curly quotes and bullets all come out
// as the same substitute character, which is what a page written by a human
// looks like when the renderer was written for a terminal.
char ascii_for_cp(uint32_t cp) {
  switch (cp) {
    case 0x00A0: return ' ';
    case 0x2002: case 0x2003: case 0x2009: case 0x200A: return ' ';
    case 0x2018: case 0x2019: case 0x201B: case 0x2032: return '\'';
    case 0x201C: case 0x201D: case 0x201F: case 0x2033: return '"';
    case 0x2013: case 0x2014: case 0x2015: case 0x2212: return '-';
    case 0x2010: case 0x2011: case 0x00AD: return '-';
    case 0x2022: case 0x00B7: case 0x2027: case 0x25CF: case 0x25AA: return '*';
    case 0x2026: return '.';
    case 0x2039: case 0x00AB: case 0x2190: return '<';
    case 0x203A: case 0x00BB: case 0x2192: return '>';
    case 0x2191: return '^';
    case 0x2193: return 'v';
    case 0x00D7: return 'x';
    case 0x00F7: return '/';
    case 0x2044: return '/';
    case 0x00A9: return 'C';
    case 0x00AE: return 'R';
    case 0x2122: return 'T';
    case 0x00B0: return 'o';
    case 0x00B1: return '+';
    case 0x2264: case 0x2265: return '=';
    case 0x2260: return '!';
    case 0x2713: case 0x2714: return 'v';
    case 0x2717: case 0x2718: return 'x';
    case 0x2605: case 0x2606: return '*';
    case 0x20AC: return 'E';
    case 0x00A3: return 'L';
    case 0x00A5: return 'Y';
    case 0x00A2: return 'c';
    case 0x2500: case 0x2501: case 0x2504: return '-';
    case 0x2502: case 0x2503: return '|';
    // Geometric shapes a dashboard reaches for: arrows for a direction or a
    // sort, squares and circles as markers. Drawn as the ASCII that means the
    // same thing rather than as a question mark that means nothing.
    case 0x25B2: case 0x25B3: case 0x25B4: case 0x25B5: case 0x2B06: case 0x1F53C: return '^';
    case 0x25BC: case 0x25BD: case 0x25BE: case 0x25BF: case 0x2B07: case 0x1F53D: return 'v';
    case 0x25C0: case 0x25C1: case 0x25C2: case 0x25C3: case 0x25C4: case 0x25C5: case 0x2B05: return '<';
    case 0x25B6: case 0x25B7: case 0x25B8: case 0x25B9: case 0x25BA: case 0x25BB: case 0x27A1: return '>';
    case 0x25A0: case 0x25A1: case 0x25A2: case 0x25A3: case 0x25AB: case 0x25AC: case 0x25FC: case 0x25FB: case 0x2B1B: case 0x2B1C: return '#';
    case 0x25CB: case 0x25CC: case 0x25CD: case 0x25CE: case 0x25EF: case 0x26AA: case 0x26AB: case 0x2B24: return 'o';
    case 0x26A0: case 0x26A1: case 0x2757: case 0x2755: return '!';
    case 0x2194: case 0x21C4: case 0x21C6: case 0x2195: return '-';
    case 0x221E: return '8';
    case 0x00B5: return 'u';
    default: break;
  }
  // Emoji and other pictographs: a marker, not a letter.
  if (cp >= 0x1F000) return '*';
  // Latin-1 letters lose their accent rather than their identity: "Ångström"
  // reads as "Angstrom", not as "?ngstr?m".
  if (cp >= 0x00C0 && cp <= 0x00C5) return 'A';
  if (cp == 0x00C7) return 'C';
  if (cp >= 0x00C8 && cp <= 0x00CB) return 'E';
  if (cp >= 0x00CC && cp <= 0x00CF) return 'I';
  if (cp == 0x00D1) return 'N';
  if (cp >= 0x00D2 && cp <= 0x00D6) return 'O';
  if (cp >= 0x00D9 && cp <= 0x00DC) return 'U';
  if (cp == 0x00DD) return 'Y';
  if (cp >= 0x00E0 && cp <= 0x00E5) return 'a';
  if (cp == 0x00E7) return 'c';
  if (cp >= 0x00E8 && cp <= 0x00EB) return 'e';
  if (cp >= 0x00EC && cp <= 0x00EF) return 'i';
  if (cp == 0x00F1) return 'n';
  if (cp >= 0x00F2 && cp <= 0x00F6) return 'o';
  if (cp >= 0x00F9 && cp <= 0x00FC) return 'u';
  if (cp == 0x00FD || cp == 0x00FF) return 'y';
  if (cp == 0x00DF) return 's';
  if (cp < 32) return ' ';
  return cp < 127 ? (char)cp : '?';
}

namespace {

struct Chosen {
  const fonts::Face* face;
  int base_px;
  bool oblique = false;   // the face is already a real italic cut
};

// The atlas face a document's font request maps onto.
//
// This used to be three lines: mono, bold, or else UI — one 15px master for the
// whole web. A 30px heading was that master upsampled 2.1x, italic text was
// drawn upright because nothing looked at f.italic, and bold code came out
// regular because the mono branch ran first. Now each family is cut at more
// than one size and the nearest is chosen, so the renderer mostly scales DOWN,
// which is the direction resampling flatters.
//
// UI_SMALL is a BOLD 11px cut used by the compositor's chrome as a label face,
// so it is deliberately not in any ramp here: small body text is UI resampled
// down, which keeps the weight right.
struct Cut { const fonts::Face* face; int px; bool oblique; };

Chosen choose_face(const web::FontKey& f) {
  // These px values are what the atlas was CUT at — see metal/tools/mkfont.py's
  // FACES. They are the ratios this picker snaps by, so a cut that moves and a
  // number here that does not is a page laid out at the wrong size.
  static const Cut kMono[]        = {{&fonts::MONO_11, 11, false}, {&fonts::MONO_12, 12, false}, {&fonts::MONO_13, 13, false}, {&fonts::MONO_14, 14, false}, {&fonts::MONO_15, 15, false}, {&fonts::MONO, 16, false}, {&fonts::MONO_21, 21, false}};
  static const Cut kBoldItalic[]  = {{&fonts::UI_BOLD_ITALIC, 17, true}};
  static const Cut kItalic[]      = {{&fonts::UI_ITALIC_13, 13, true}, {&fonts::UI_ITALIC_14, 14, true}, {&fonts::UI_ITALIC_15, 15, true}, {&fonts::UI_ITALIC_16, 16, true}, {&fonts::UI_ITALIC, 17, true}, {&fonts::UI_ITALIC_21, 21, true}};
  static const Cut kBold[]        = {{&fonts::UI_SMALL, 12, false}, {&fonts::UI_BOLD_13, 13, false}, {&fonts::UI_BOLD_14, 14, false}, {&fonts::UI_BOLD_15, 15, false}, {&fonts::UI_BOLD_16, 16, false}, {&fonts::UI_BOLD, 17, false}, {&fonts::UI_BOLD_19, 19, false}, {&fonts::UI_BOLD_21, 21, false}, {&fonts::UI_BOLD_24, 24, false}, {&fonts::UI_BOLD_30, 30, false}};
  static const Cut kRegular[]     = {{&fonts::UI_11, 11, false}, {&fonts::UI_12, 12, false}, {&fonts::UI_13, 13, false}, {&fonts::UI_14, 14, false}, {&fonts::UI_15, 15, false}, {&fonts::UI_16, 16, false}, {&fonts::UI, 17, false}, {&fonts::UI_19, 19, false}, {&fonts::UI_21, 21, false}, {&fonts::UI_24, 24, false}, {&fonts::UI_30, 30, false}};

  static const Cut kSerif[]       = {{&fonts::SERIF_13, 13, false}, {&fonts::SERIF_14, 14, false}, {&fonts::SERIF_15, 15, false}, {&fonts::SERIF_16, 16, false}, {&fonts::SERIF, 17, false}, {&fonts::SERIF_19, 19, false}, {&fonts::SERIF_24, 24, false}, {&fonts::SERIF_30, 30, false}};
  static const Cut kSerifBold[]   = {{&fonts::SERIF_BOLD_14, 14, false}, {&fonts::SERIF_BOLD_16, 16, false}, {&fonts::SERIF_BOLD, 17, false}, {&fonts::SERIF_BOLD_19, 19, false}, {&fonts::SERIF_BOLD_24, 24, false}, {&fonts::SERIF_BOLD_30, 30, false}};
  static const Cut kSerifItalic[] = {{&fonts::SERIF_ITALIC_14, 14, true}, {&fonts::SERIF_ITALIC_16, 16, true}, {&fonts::SERIF_ITALIC, 17, true}};
  static const Cut kSerifBI[]     = {{&fonts::SERIF_BOLD_ITALIC, 17, true}};

  const Cut* cuts = kRegular;
  size_t n = sizeof(kRegular) / sizeof(kRegular[0]);
  if (f.mono) { cuts = kMono; n = sizeof(kMono) / sizeof(kMono[0]); }
  else if (f.serif) {
    if (f.bold && f.italic) { cuts = kSerifBI; n = 1; }
    else if (f.italic) { cuts = kSerifItalic; n = sizeof(kSerifItalic) / sizeof(kSerifItalic[0]); }
    else if (f.bold) { cuts = kSerifBold; n = sizeof(kSerifBold) / sizeof(kSerifBold[0]); }
    else { cuts = kSerif; n = sizeof(kSerif) / sizeof(kSerif[0]); }
  }
  else if (f.bold && f.italic) { cuts = kBoldItalic; n = sizeof(kBoldItalic) / sizeof(kBoldItalic[0]); }
  else if (f.italic) { cuts = kItalic; n = sizeof(kItalic) / sizeof(kItalic[0]); }
  else if (f.bold) { cuts = kBold; n = sizeof(kBold) / sizeof(kBold[0]); }

  const Cut* best = &cuts[0];
  float best_err = 1e9f;
  const float want = f.px > 0 ? f.px : 16.0f;
  for (size_t i = 0; i < n; ++i) {
    float diff = std::abs(want - (float)cuts[i].px);
    if (diff < best_err) { best_err = diff; best = &cuts[i]; }
  }
  return {best->face, best->px, best->oblique};
}

int round_i(float v) { return (int)(v < 0 ? v - 0.5f : v + 0.5f); }

// Walk a UTF-8 string one drawable character at a time.
// Walks the text as CODE POINTS. It used to transliterate here, handing the
// renderer a char — which meant an accent could never be drawn even once the
// atlas carried it, because the information was thrown away one layer too
// early. Transliteration is now what glyph_for() does when the face misses.
struct CharWalk {
  const std::string& s;
  size_t i = 0;
  explicit CharWalk(const std::string& src) : s(src) {}
  bool next(uint32_t& out) {
    if (i >= s.size()) return false;
    unsigned char c = (unsigned char)s[i];
    if ((c & 0x80) == 0) {
      out = c;
      ++i;
      return true;
    }
    uint32_t cp = 0;
    int extra = 0;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; extra = 3; }
    else { ++i; out = '?'; return true; }
    ++i;
    for (int e = 0; e < extra && i < s.size(); ++e) cp = (cp << 6) | ((unsigned char)s[i++] & 0x3Fu);
    out = cp;
    return true;
  }
};

// The glyph a code point draws as: the face's own if it has one, otherwise the
// honest ASCII stand-in. Two steps, in that order, so widening the atlas is all
// it takes for a character to start being drawn properly.
// A glyph and the blob its rows live in — they travel together now, because a
// glyph cut at runtime carries its own coverage rather than an offset into the
// face's.
struct Drawn {
  const fonts::Glyph* g;
  const uint8_t* cov;
};

Drawn glyph_for(const fonts::Face& face, uint32_t cp) {
  if (const fonts::Glyph* g = face.find(cp)) return {g, face.cov};
  // A page is written in whatever alphabet its author writes in. The atlas
  // carries Latin; the outline engine cuts the rest on demand, so a Greek or
  // Cyrillic or Han page renders as itself instead of as a row of '?'.
  if (const glyphs::Rasterized* r = glyphs::rasterize(face, cp)) return {&r->g, r->cov};
  return {&face.at(ascii_for_cp(cp)), face.cov};
}

// Coverage at a fractional source position, bilinear. Sampling outside the
// inked box reads zero, which is what makes a resampled glyph's edge soft
// instead of clipped square.
int cov_at(const uint8_t* cov, const fonts::Glyph& g, float sx, float sy) {
  int x0 = (int)(sx < 0 ? sx - 1 : sx);
  int y0 = (int)(sy < 0 ? sy - 1 : sy);
  float fx = sx - (float)x0;
  float fy = sy - (float)y0;
  auto at = [&](int x, int y) -> int {
    if (x < 0 || y < 0 || x >= (int)g.w || y >= (int)g.h) return 0;
    return cov[g.off + (size_t)y * g.w + (size_t)x];
  };
  float c00 = (float)at(x0, y0), c10 = (float)at(x0 + 1, y0);
  float c01 = (float)at(x0, y0 + 1), c11 = (float)at(x0 + 1, y0 + 1);
  float top = c00 + (c10 - c00) * fx;
  float bot = c01 + (c11 - c01) * fx;
  float v = top + (bot - top) * fy;
  int out = round_i(v);
  return out < 0 ? 0 : out > 255 ? 255 : out;
}

}  // namespace

// ------------------------------------------------------------------- type --

int web_line_height(const web::FontKey& f) {
  Chosen ch = choose_face(f);
  int h = round_i((float)ch.face->line_h * (float)f.px / (float)ch.base_px);
  return h < 6 ? 6 : h;
}

int web_ascent(const web::FontKey& f) {
  Chosen ch = choose_face(f);
  int a = round_i((float)ch.face->ascent * (float)f.px / (float)ch.base_px);
  return a < 5 ? 5 : a;
}

constexpr float kAdvanceFit = 0.94f;

// The pair adjustment between two neighbours (font_kerning.hpp), fitted the way the advances are. A monospace face
// is not kerned, nor is anything next to a space.
float kern_adj(uint32_t left, uint32_t right, const web::FontKey& f) {
  if (f.mono || left == ' ' || right == ' ' || left == 0x00A0 || right == 0x00A0) return 0.0f;
  using fonts::KernFamily;
  const KernFamily fam = f.serif ? (f.bold ? (f.italic ? KernFamily::SerifBoldItalic : KernFamily::SerifBold)
                                           : (f.italic ? KernFamily::SerifItalic : KernFamily::Serif))
                                 : (f.bold ? (f.italic ? KernFamily::SansBoldItalic : KernFamily::SansBold)
                                           : (f.italic ? KernFamily::SansItalic : KernFamily::Sans));
  return fonts::kern_px(fam, left, right, f.px) * kAdvanceFit;
}

float glyph_adv(const fonts::Glyph& g, uint32_t cp, const web::FontKey& f, float scale) {
  if (f.mono) return (float)g.adv * scale + f.tracking;
  if (cp == ' ' || cp == 0x00A0) {
    // Word spacing: standard browser space is ~0.26em, whereas DejaVu Sans is cut
    // at ~0.32em, tracking lines wide across runs of words.
    float sp = std::round(0.26f * f.px);
    if (sp < 3.0f) sp = 3.0f;
    return sp + f.tracking;
  }
  // DejaVu Sans glyph advances are metrically wide (~8.25px vs Chrome's ~7.3px at 16px).
  // Scaling advances by ~0.94 matches standard browser proportions so lines are not tracked wide.
  float a = (float)g.adv * scale * kAdvanceFit;
  float min_adv = (float)g.w * scale;
  if (a < min_adv) a = min_adv;
  return a + f.tracking;
}

int web_text_width(const std::string& text, const web::FontKey& f) {
  Chosen ch = choose_face(f);
  float scale = (float)f.px / (float)ch.base_px;
  CharWalk walk(text);
  uint32_t cp;
  float w = 0;
  uint32_t prev = 0;
  while (walk.next(cp)) {
    if (cp == '\n') break;
    if (prev) w += kern_adj(prev, cp, f);
    w += glyph_adv(*glyph_for(*ch.face, cp).g, cp, f, scale);
    prev = cp;
  }
  // Rounded UP, not to nearest. A width is asked for in order to fit text into
  // it, and a measurement that rounds down by half a pixel per run makes a box
  // that its own content does not fit in -- with nine runs in a row, the usage
  // pill came out two pixels short and wrapped its last segment onto a second
  // line.
  return (int)(w + 0.999f);
}

// Glyph coverage is shaped so faint fringe pixels are pulled down while the core
// stroke is kept crisp, matching Chrome and the compositor's font_cov_smooth.
static int dark_cov(int a) {
  static uint8_t t[256]; static bool init = false;
  if (!init) {
    double gamma = 1.25;
    if (const char* e = std::getenv("AIOS_FONT_GAMMA")) {
      double g = std::atof(e);
      if (g > 0.2 && g < 4.0) gamma = g;
    }
    for (int i = 0; i < 256; ++i) {
      t[i] = (uint8_t)(255.0 * std::pow(i / 255.0, gamma) + 0.5);
    }
    init = true;
  }
  return t[a & 255];
}

void web_draw_text_px(std::vector<uint32_t>& buf, int bw, int bh, int x, int y,
                      const std::string& text, uint32_t color, const web::FontKey& f,
                      const DrawClip& clip) {
  Chosen ch = choose_face(f);
  const fonts::Face& face = *ch.face;
  float scale = (float)f.px / (float)ch.base_px;
  int alpha_base = (int)((color >> 24) & 0xFF);
  if (!alpha_base) alpha_base = 255;

  // Within a hair of the face's own size, stamp the coverage straight through.
  // Resampling at 1.0 is not free and it is not lossless either.
  bool exact = scale > 0.97f && scale < 1.03f;
  float pen = (float)x;
  CharWalk walk(text);
  uint32_t glyph_cp;
  uint32_t prev_cp = 0;
  while (walk.next(glyph_cp)) {
    if (glyph_cp == '\n') break;
    if (prev_cp) pen += kern_adj(prev_cp, glyph_cp, f);
    prev_cp = glyph_cp;
    const Drawn drawn = glyph_for(face, glyph_cp);
    const fonts::Glyph& g = *drawn.g;
    if (g.w && g.h && drawn.cov) {
      if (exact) {
        int gx = round_i(pen) + g.bx;
        int gy = y + g.by;
        for (int row = 0; row < (int)g.h; ++row) {
          int py = gy + row;
          if (py < 0 || py >= bh) continue;
          for (int col = 0; col < (int)g.w; ++col) {
            int px = gx + col;
            if (px < 0 || px >= bw) continue;
            if (!clip.has(px, py)) continue;
            int a = dark_cov(drawn.cov[g.off + (size_t)row * g.w + (size_t)col]);
            if (a) px::blend(buf, bw, bh, px, py, color, a * alpha_base / 255);
          }
        }
      } else {
        int tw = (int)((float)g.w * scale + 0.999f);
        int th = (int)((float)g.h * scale + 0.999f);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        int gx = round_i(pen + (float)g.bx * scale);
        int gy = y + round_i((float)g.by * scale);
        // A shear ONLY where the chosen face is not already an italic cut.
        // The atlas has real obliques now; shearing one of those as well drew
        // every off-size italic at twice the slant it should have.
        float slant = (f.italic && !ch.oblique) ? 0.21f : 0.0f;
        for (int row = 0; row < th; ++row) {
          int py = gy + row;
          if (py < 0 || py >= bh) continue;
          float sy = ((float)row + 0.5f) / scale - 0.5f;
          int shear = slant > 0 ? (int)(slant * (float)(th - row)) : 0;
          for (int col = 0; col < tw; ++col) {
            int px = gx + col + shear;
            if (px < 0 || px >= bw) continue;
            if (!clip.has(px, py)) continue;
            float sx = ((float)col + 0.5f) / scale - 0.5f;
            int a = dark_cov(cov_at(drawn.cov, g, sx, sy));
            if (a) px::blend(buf, bw, bh, px, py, color, a * alpha_base / 255);
          }
        }
      }
    }
    pen += glyph_adv(g, glyph_cp, f, scale);
  }
}

std::string display_glyph_gaps(const std::string& s) {
  // Code points the atlas cannot draw AND the table above has no honest
  // stand-in for -- what comes out as '?'. Each reported once.
  //
  // The atlas is asked FIRST. It used to be asked not at all: this reported
  // everything ascii_for_cp had no entry for, which was the right list when the
  // faces stopped at ASCII and is far too long now that they carry Latin-1 and
  // the punctuation a page is written with. A face carrying the glyph is not a
  // gap, whatever the transliteration table thinks.
  std::string out;
  std::vector<uint32_t> seen;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    uint32_t cp = 0;
    int extra = 0;
    if ((c & 0x80) == 0) { ++i; continue; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; extra = 3; }
    else { ++i; continue; }
    const size_t start = i;
    ++i;
    for (int e = 0; e < extra && i < s.size(); ++e) cp = (cp << 6) | ((unsigned char)s[i++] & 0x3Fu);
    if (fonts::UI.find(cp)) continue;          // the atlas draws it
    if (glyphs::rasterize(fonts::UI, cp)) continue;  // the outline engine cuts it
    if (ascii_for_cp(cp) != '?') continue;     // an honest stand-in exists
    bool dup = false;
    for (uint32_t v : seen) if (v == cp) { dup = true; break; }
    if (dup) continue;
    seen.push_back(cp);
    if (!out.empty()) out += ' ';
    out += s.substr(start, i - start);
  }
  return out;
}

}  // namespace aios::userland
