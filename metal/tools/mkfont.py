#!/usr/bin/env python3
"""Rasterise real typefaces into an antialiased atlas the kernel can draw.

The bare-metal desktop used to draw text with a 1-bit 8x16 VGA font: every
stem one pixel wide, every curve a staircase, every glyph the same width. That
is what made a 32bpp truecolour desktop look like a 16-bit one. There is no
FreeType on the metal and no room for a TrueType interpreter in a freestanding
kernel, so the rasterising happens HERE, at build time, with a real font
engine — and what ships is 8-bit coverage per pixel, which the compositor
blends. Same result as a hinted glyph on a hosted desktop, without a font
engine in ring 0.

Two things this atlas grew when the browser started rendering real pages:

  A SIZE RAMP. There used to be one 15px master, and a heading at 32px was that
  master upsampled 2.1x — soft stems, mushy counters, every large word on the
  web slightly out of focus. Now each family is cut at several sizes and the
  renderer picks the nearest, so it mostly scales DOWN, which is the direction
  resampling flatters.

  COVERAGE PAST ASCII. `at(char)` could only ever index 32..126, so every
  accent, curly quote, dash and arrow on a page went through a transliteration
  table on its way to the screen. The ASCII block is still contiguous and
  indexed directly; everything else is a sorted sparse table the renderer binary
  searches, and the transliteration is what happens when even that misses.

Writes three artefacts from one run, so they cannot disagree:
  include/aios/userland/font_atlas.hpp   the shape, and the faces declared
  src/userland/font_atlas.cpp            the data (a megabyte does not belong
                                         in a header every TU re-parses)
  metal/font_atlas.json                  the same glyphs for metal/drive-desktop.py,
                                         which reads text back off the screen
Usage: mkfont.py [--check]      (--check regenerates and diffs, changing nothing)
"""
import json
import os
import sys
import zlib

from PIL import Image, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

# Where the TTFs live on a Debian/Ubuntu box. AIOS_DEJAVU_DIR points this at
# any other directory carrying the same faces, which is what lets the atlas be
# regenerated somewhere that is not a Linux host.
DEJAVU = os.environ.get("AIOS_DEJAVU_DIR", "/usr/share/fonts/truetype/dejavu")

# DejaVu is metrically generous and hints well at small sizes; its Sans is the
# closest widely-installed face to the console's system-sans stack, and its
# Sans Mono is a real programmer's mono rather than a terminal ROM font.
#
# The sizes are chosen so that the ratio between neighbours is about 1.4: the
# renderer snaps a request to the nearest cut, so the worst resample on the page
# is ~1.2x in either direction instead of 2.1x up. UI_SMALL stays a BOLD 11px
# cut because the compositor's chrome uses it as a label face, not as small body
# text.
# The base cuts were 15px sans and 14px mono. On a 1920x1080 panel that is
# small enough that a stem is about a pixel and a half wide, which no amount of
# correct rasterising makes crisp — the glyph simply has not got the pixels.
# Seventeen and sixteen put two pixels in a stem, which is the difference
# between text you read and text you decode. UI_SMALL is a label face, not body
# text, and moves by one.
FACES = [
    ("UI",             "%s/DejaVuSans.ttf" % DEJAVU,           17),
    ("UI_21",          "%s/DejaVuSans.ttf" % DEJAVU,           21),
    ("UI_30",          "%s/DejaVuSans.ttf" % DEJAVU,           30),
    ("UI_BOLD",        "%s/DejaVuSans-Bold.ttf" % DEJAVU,      17),
    ("UI_BOLD_21",     "%s/DejaVuSans-Bold.ttf" % DEJAVU,      21),
    ("UI_BOLD_30",     "%s/DejaVuSans-Bold.ttf" % DEJAVU,      30),
    ("UI_ITALIC",      "%s/DejaVuSans-Oblique.ttf" % DEJAVU,   17),
    ("UI_ITALIC_21",   "%s/DejaVuSans-Oblique.ttf" % DEJAVU,   21),
    ("UI_BOLD_ITALIC", "%s/DejaVuSans-BoldOblique.ttf" % DEJAVU, 17),
    ("UI_SMALL",       "%s/DejaVuSans-Bold.ttf" % DEJAVU,      12),
    ("MONO",           "%s/DejaVuSansMono.ttf" % DEJAVU,       16),
    ("MONO_21",        "%s/DejaVuSansMono.ttf" % DEJAVU,       21),
    # The serif family, so `font-family: Georgia, serif` (every encyclopedia
    # heading) is not drawn in a sans. Same px as the sans cuts they replace.
    ("SERIF",             "%s/DejaVuSerif.ttf" % DEJAVU,            17),
    ("SERIF_30",          "%s/DejaVuSerif.ttf" % DEJAVU,            30),
    ("SERIF_BOLD",        "%s/DejaVuSerif-Bold.ttf" % DEJAVU,       17),
    ("SERIF_BOLD_30",     "%s/DejaVuSerif-Bold.ttf" % DEJAVU,       30),
    ("SERIF_ITALIC",      "%s/DejaVuSerif-Italic.ttf" % DEJAVU,     17),
    ("SERIF_BOLD_ITALIC", "%s/DejaVuSerif-BoldItalic.ttf" % DEJAVU, 17),
    # The web ramp: page text is mostly 11-16px, and a 17px cut resampled down to
    # 13 is soft and pale. Cut at the sizes pages actually use so it is stamped 1:1.
    ("UI_11", "%s/DejaVuSans.ttf" % DEJAVU, 11),
    ("UI_12", "%s/DejaVuSans.ttf" % DEJAVU, 12),
    ("UI_13", "%s/DejaVuSans.ttf" % DEJAVU, 13),
    ("UI_14", "%s/DejaVuSans.ttf" % DEJAVU, 14),
    ("UI_15", "%s/DejaVuSans.ttf" % DEJAVU, 15),
    ("UI_16", "%s/DejaVuSans.ttf" % DEJAVU, 16),
    ("UI_19", "%s/DejaVuSans.ttf" % DEJAVU, 19),
    ("UI_24", "%s/DejaVuSans.ttf" % DEJAVU, 24),
    ("UI_BOLD_13", "%s/DejaVuSans-Bold.ttf" % DEJAVU, 13),
    ("UI_BOLD_14", "%s/DejaVuSans-Bold.ttf" % DEJAVU, 14),
    ("UI_BOLD_15", "%s/DejaVuSans-Bold.ttf" % DEJAVU, 15),
    ("UI_BOLD_16", "%s/DejaVuSans-Bold.ttf" % DEJAVU, 16),
    ("UI_BOLD_19", "%s/DejaVuSans-Bold.ttf" % DEJAVU, 19),
    ("UI_BOLD_24", "%s/DejaVuSans-Bold.ttf" % DEJAVU, 24),
    ("UI_ITALIC_13", "%s/DejaVuSans-Oblique.ttf" % DEJAVU, 13),
    ("UI_ITALIC_14", "%s/DejaVuSans-Oblique.ttf" % DEJAVU, 14),
    ("UI_ITALIC_15", "%s/DejaVuSans-Oblique.ttf" % DEJAVU, 15),
    ("UI_ITALIC_16", "%s/DejaVuSans-Oblique.ttf" % DEJAVU, 16),
    ("SERIF_13", "%s/DejaVuSerif.ttf" % DEJAVU, 13),
    ("SERIF_14", "%s/DejaVuSerif.ttf" % DEJAVU, 14),
    ("SERIF_15", "%s/DejaVuSerif.ttf" % DEJAVU, 15),
    ("SERIF_16", "%s/DejaVuSerif.ttf" % DEJAVU, 16),
    ("SERIF_19", "%s/DejaVuSerif.ttf" % DEJAVU, 19),
    ("SERIF_24", "%s/DejaVuSerif.ttf" % DEJAVU, 24),
    ("SERIF_BOLD_14", "%s/DejaVuSerif-Bold.ttf" % DEJAVU, 14),
    ("SERIF_BOLD_16", "%s/DejaVuSerif-Bold.ttf" % DEJAVU, 16),
    ("SERIF_BOLD_19", "%s/DejaVuSerif-Bold.ttf" % DEJAVU, 19),
    ("SERIF_BOLD_24", "%s/DejaVuSerif-Bold.ttf" % DEJAVU, 24),
    ("SERIF_ITALIC_14", "%s/DejaVuSerif-Italic.ttf" % DEJAVU, 14),
    ("SERIF_ITALIC_16", "%s/DejaVuSerif-Italic.ttf" % DEJAVU, 16),
    ("MONO_11", "%s/DejaVuSansMono.ttf" % DEJAVU, 11),
    ("MONO_12", "%s/DejaVuSansMono.ttf" % DEJAVU, 12),
    ("MONO_13", "%s/DejaVuSansMono.ttf" % DEJAVU, 13),
    ("MONO_14", "%s/DejaVuSansMono.ttf" % DEJAVU, 14),
    ("MONO_15", "%s/DejaVuSansMono.ttf" % DEJAVU, 15),
]
FIRST, LAST = 32, 126

# Everything past ASCII this atlas carries, as code points. Latin-1 in full,
# because it is what European prose is spelled with; then the punctuation a page
# written by a person is actually full of, the arrows a navigation uses as
# glyphs, and the handful of maths signs that turn up in running text.
EXTRA_CPS = (
    list(range(0x00A0, 0x0100)) +                       # Latin-1 supplement
    # Latin Extended-A, whole. The Latin-1 supplement covers French, German,
    # Spanish and the Nordics and then stops dead: Polish, Czech, Slovak,
    # Hungarian, Romanian, Turkish, Latvian and Lithuanian all live up here.
    # Without it a name as ordinary as Łukasz or Dvořák came off the atlas as
    # question marks, which is what a missing glyph looks like to a reader.
    list(range(0x0100, 0x0180)) +
    [0x0192] +
    [0x2010, 0x2011, 0x2013, 0x2014, 0x2015,            # hyphens and dashes
     0x2018, 0x2019, 0x201A, 0x201B,                    # single quotes
     0x201C, 0x201D, 0x201E, 0x201F,                    # double quotes
     0x2020, 0x2021, 0x2022, 0x2026, 0x2030,            # daggers, bullet, ellipsis
     0x2032, 0x2033, 0x2039, 0x203A, 0x203B,
     0x20AC,                                            # euro
     0x2122,                                            # trademark
     0x2190, 0x2191, 0x2192, 0x2193, 0x2194, 0x2195,    # arrows
     0x21A9, 0x21AA, 0x21B5, 0x21C4, 0x21C6,
     0x2212, 0x2213, 0x2248, 0x2260, 0x2261,            # maths
     0x2264, 0x2265, 0x221E, 0x221A, 0x2211, 0x220F,
     0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518,    # box drawing, for pre
     0x251C, 0x2524, 0x252C, 0x2534, 0x253C,
     0x2588, 0x2591, 0x2592, 0x2593,
     0x25A0, 0x25A1, 0x25AA, 0x25AB, 0x25B2, 0x25B6,    # geometric markers
     0x25BC, 0x25C0, 0x25CB, 0x25CF, 0x25CA,
     0x2605, 0x2606, 0x2610, 0x2611, 0x2612,
     0x2713, 0x2714, 0x2717, 0x2718,                    # ticks and crosses
     0x26A0, 0x2764]
)


def render(font, code):
    """One glyph's box, advance and 8-bit coverage.

    The coverage is FreeType's own hinted, antialiased mask at the target size.

    It used to be a 4x oversample boxed back down, on the reasoning that this is
    "what gives a stem its grey edge instead of a hard one". It gives every stem
    a grey edge — including the ones that land exactly on a pixel boundary and
    should be solid. Measured on DejaVu Sans Mono 14px, the right stem of 'n' is
    a single 255 column when hinted and a 118/175 smear across two columns when
    oversampled. Body text made of smeared stems is what reads as out of focus.

    The hinted mask is still antialiased — mode="L" is 8-bit coverage, not a
    bilevel bitmap — so curves and diagonals keep their grey. What changes is
    that vertical and horizontal stems snap to the pixel grid, which is the
    whole job of hinting and the reason text on every other desktop is crisp at
    these sizes.
    """
    ch = chr(code)
    adv = int(round(font.getlength(ch)))
    mask = font.getmask(ch, mode="L")
    w, h = mask.size
    bbox = font.getbbox(ch)                 # (x0, y0, x1, y1) from the line top
    bx = int(bbox[0]) if bbox else 0
    by = int(bbox[1]) if bbox else 0

    pix = bytes(mask) if w and h else b""

    if w * h != len(pix):                   # a blank glyph (space)
        w = h = 0
        pix = b""
    # The struct holds these in a byte apiece; a glyph that will not fit is
    # dropped rather than silently truncated into a smear at the wrong offset.
    if w > 255 or h > 255 or adv > 255 or not (-128 <= bx <= 127) or not (-128 <= by <= 127):
        return None
    return {"code": code, "adv": adv, "bx": bx, "by": by, "w": w, "h": h, "pix": pix}


def build_face(path, px):
    font = ImageFont.truetype(path, px)
    ascent, descent = font.getmetrics()
    glyphs, extra = [], []
    blob = bytearray()

    for code in range(FIRST, LAST + 1):
        g = render(font, code)
        if g is None:                       # never for ASCII, but the block must stay dense
            g = {"code": code, "adv": px // 2, "bx": 0, "by": 0, "w": 0, "h": 0, "pix": b""}
        g["off"] = len(blob)
        blob.extend(g["pix"])
        glyphs.append(g)

    for code in sorted(set(EXTRA_CPS)):
        # A face that has no glyph for a code point must not contribute an empty
        # box for it: the renderer has to MISS, so the transliteration below it
        # gets its turn.
        if not font.getmask(chr(code), mode="L").size[0] and chr(code).strip():
            continue
        g = render(font, code)
        if g is None:
            continue
        g["off"] = len(blob)
        blob.extend(g["pix"])
        extra.append(g)

    return {"ascent": ascent, "line_h": ascent + descent, "px": px,
            "glyphs": glyphs, "extra": extra, "blob": bytes(blob),
            "source": os.path.basename(path)}


HEADER_TOP = '''// GENERATED by metal/tools/mkfont.py — do not edit by hand.
//
// Real typefaces, rasterised at build time by FreeType, shipped as 8-bit
// coverage. The kernel blends this against whatever is under the glyph, which
// is why text on the bare-metal desktop has the same edges as text on the
// console: antialiasing is a property of the coverage, not of the display.
//
// The data lives in src/userland/font_atlas.cpp. It is a megabyte, and a
// megabyte of constexpr in a header is a megabyte every translation unit that
// includes it has to re-parse. Each face's coverage is kept deflated there
// (3.4 MB of mostly 0 and 255 is 0.8 MB deflated) and inflated the first time
// the face is drawn with (cov(), src/userland/font_cov.cpp): a binary that
// never draws text - a server - never pays for it in memory either.
#pragma once
#include <cstdint>

namespace aios::userland::fonts {

struct Glyph {
  uint32_t off;   // into the face's coverage blob
  uint8_t w, h;   // the inked box, which is smaller than the advance
  int8_t bx, by;  // where that box sits relative to the pen and the line top
  uint8_t adv;    // how far the pen moves
};

// A glyph the face carries that is not in the contiguous ASCII block. Sorted by
// code point, so a lookup is a binary search rather than a scan of a thousand.
struct CpGlyph {
  uint32_t cp;
  Glyph g;
};

struct Face {
  const Glyph* glyphs;
  const uint8_t* covz;   // the coverage blob, raw deflate
  uint8_t ascent;   // line top to baseline
  uint8_t line_h;   // ascent + descent: one row of text
  uint8_t first, count;
  const CpGlyph* extra;
  uint16_t extra_n;
  uint8_t px;       // the size this face was cut at
  uint32_t covz_n;  // deflated bytes
  uint32_t cov_n;   // the blob's bytes, inflated
  const uint8_t** slot;   // the inflated blob, once a glyph of the face has been drawn
  // The coverage blob a Glyph's `off` indexes: inflated on the first call (once, whichever thread gets there)
  // and kept for the life of the program.
  const uint8_t* cov() const;
  const Glyph& at(char c) const {
    unsigned char u = static_cast<unsigned char>(c);
    if (u < first || u >= first + count) u = ' ';
    return glyphs[u - first];
  }
  // The glyph for a code point, or nullptr if this face has none. Nullptr is
  // the answer that lets the caller fall back to a transliteration rather than
  // drawing a box or a blank where a letter should be.
  const Glyph* find(uint32_t cp) const {
    if (cp >= first && cp < (uint32_t)first + count) return &glyphs[cp - first];
    uint32_t lo = 0, hi = extra_n;
    while (lo < hi) {
      uint32_t mid = lo + (hi - lo) / 2;
      if (extra[mid].cp == cp) return &extra[mid].g;
      if (extra[mid].cp < cp) lo = mid + 1; else hi = mid;
    }
    return nullptr;
  }
};

'''

HEADER_BOTTOM = '''
// Every face, by name: for a check that walks them all (tools/font-atlas-check.cpp).
struct NamedFace {
  const char* name;
  const Face* face;
};
extern const NamedFace kFaces[];
extern const uint32_t kFaceCount;

}  // namespace aios::userland::fonts
'''


def emit_header(faces):
    out = [HEADER_TOP]
    for name, face in faces.items():
        out.append("// %s: %s at %dpx, ascent %d, line %d, %d glyphs past ASCII" %
                   (name, face["source"], face["px"], face["ascent"], face["line_h"],
                    len(face["extra"])))
        out.append("extern const Face %s;" % name)
    out.append(HEADER_BOTTOM)
    return "\n".join(out)


def emit_source(faces):
    out = []
    w = out.append
    w("// GENERATED by metal/tools/mkfont.py — do not edit by hand.")
    w("//")
    w("// The atlas data. Freestanding by construction: <cstdint> and nothing")
    w("// else, so this compiles into the bare-metal image as readily as into the")
    w("// hosted one.")
    w('#include "aios/userland/font_atlas.hpp"')
    w("")
    w("namespace aios::userland::fonts {")
    w("")
    for name, face in faces.items():
        w("// ---- %s: %s at %dpx" % (name, face["source"], face["px"]))
        w("static const Glyph %s_GLYPHS[] = {" % name)
        for g in face["glyphs"]:
            w("  {%d,%d,%d,%d,%d,%d},  // %d %s" %
              (g["off"], g["w"], g["h"], g["bx"], g["by"], g["adv"], g["code"],
               repr(chr(g["code"]))))
        w("};")
        if face["extra"]:
            w("static const CpGlyph %s_EXTRA[] = {" % name)
            for g in face["extra"]:
                w("  {0x%04X,{%d,%d,%d,%d,%d,%d}}," %
                  (g["code"], g["off"], g["w"], g["h"], g["bx"], g["by"], g["adv"]))
            w("};")
        else:
            w("static const CpGlyph* const %s_EXTRA = nullptr;" % name)
        blob = bytes(face["blob"])
        co = zlib.compressobj(9, zlib.DEFLATED, -15, 9)   # raw deflate: net::inflate_raw_exact reads it
        packed = co.compress(blob) + co.flush()
        w("// %d bytes of coverage, %d deflated" % (len(blob), len(packed)))
        w("static const uint8_t %s_COVZ[] = {" % name)
        for i in range(0, len(packed), 32):
            w("  " + ",".join(str(b) for b in packed[i:i + 32]) + ",")
        w("};")
        w("static const uint8_t* %s_SLOT = nullptr;" % name)
        w("const Face %s = {%s_GLYPHS, %s_COVZ, %d, %d, %d, %d, %s, %d, %d, %d, %d, &%s_SLOT};" %
          (name, name, name, face["ascent"], face["line_h"], FIRST, LAST - FIRST + 1,
           ("%s_EXTRA" % name) if face["extra"] else "nullptr",
           len(face["extra"]), face["px"], len(packed), len(blob), name))
        w("")
    w("const NamedFace kFaces[] = {")
    for name in faces:
        w('  {"%s", &%s},' % (name, name))
    w("};")
    w("const uint32_t kFaceCount = %d;" % len(faces))
    w("")
    w("}  // namespace aios::userland::fonts")
    return "\n".join(out) + "\n"


def emit_json(faces):
    doc = {}
    for name, face in faces.items():
        entries = {}
        for g in list(face["glyphs"]) + list(face["extra"]):
            entries[str(g["code"])] = {"adv": g["adv"], "bx": g["bx"], "by": g["by"],
                                       "w": g["w"], "h": g["h"], "pix": list(g["pix"])}
        doc[name] = {"px": face["px"], "ascent": face["ascent"], "line_h": face["line_h"],
                     "source": face["source"], "glyphs": entries}
    return json.dumps(doc, separators=(",", ":")) + "\n"


def main():
    missing = [p for _, p, _ in FACES if not os.path.exists(p)]
    if missing:
        print("mkfont: missing font file(s): " + ", ".join(missing), file=sys.stderr)
        return 1
    faces = {name: build_face(path, px) for name, path, px in FACES}
    header = emit_header(faces)
    source = emit_source(faces)
    blob = emit_json(faces)
    hpath = os.path.join(ROOT, "include", "aios", "userland", "font_atlas.hpp")
    cpath = os.path.join(ROOT, "src", "userland", "font_atlas.cpp")
    jpath = os.path.join(ROOT, "metal", "font_atlas.json")
    if "--check" in sys.argv:
        bad = False
        for path, want in ((hpath, header), (cpath, source), (jpath, blob)):
            have = open(path).read() if os.path.exists(path) else ""
            if have != want:
                print("mkfont: %s is not what this script would generate" % path)
                bad = True
        if not bad:
            print("mkfont: atlas is current")
        return 1 if bad else 0
    open(hpath, "w").write(header)
    open(cpath, "w").write(source)
    open(jpath, "w").write(blob)
    total = sum(len(f["blob"]) for f in faces.values())
    print("mkfont: %d faces, %.1f KiB of coverage" % (len(faces), total / 1024.0))
    for name, f in faces.items():
        print("  %-15s %-26s %2dpx  ascent %2d  line %2d  +%d past ascii" %
              (name, f["source"], f["px"], f["ascent"], f["line_h"], len(f["extra"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
