// The glyph tables a browser build does not carry. font_atlas.cpp is 3.9 MB of rasterised DejaVu and Liberation
// coverage for painting text into pixels (the desktop, a canvas pane, a PNG); a page draws its own text, and the
// markup a pane produces never touches them. So every face is here with its real size and line metrics and ONE blank
// glyph: text measures at about half its size per character, and painting it puts nothing on the picture. Pixel
// painting of text is therefore not available in a browser build, and a program that needs it says so by showing
// no text rather than wrong text.
//
// Generated from the `// NAME: ... at Npx, ascent A, line L` lines of include/aios/userland/font_atlas.hpp; a face
// added there that is missing here is a link error, not a silent gap.
#include "aios/userland/font_atlas.hpp"

namespace aios::userland::fonts {

namespace {
const uint8_t kNoCoverage[1] = {0};
}

#define BLANK_FACE(name, size, ascent_, line)                                                  \
  static const Glyph name##_blank{0, 0, 0, 0, 0, static_cast<uint8_t>((size) / 2)};            \
  const Face name{&name##_blank, kNoCoverage, ascent_, line, 32, 1, nullptr, 0, size};

BLANK_FACE(UI, 17, 16, 21)
BLANK_FACE(UI_21, 21, 20, 25)
BLANK_FACE(UI_30, 30, 28, 36)
BLANK_FACE(UI_BOLD, 17, 16, 21)
BLANK_FACE(UI_BOLD_21, 21, 20, 25)
BLANK_FACE(UI_BOLD_30, 30, 28, 36)
BLANK_FACE(UI_ITALIC, 17, 16, 21)
BLANK_FACE(UI_ITALIC_21, 21, 20, 25)
BLANK_FACE(UI_BOLD_ITALIC, 17, 16, 21)
BLANK_FACE(UI_SMALL, 12, 12, 15)
BLANK_FACE(MONO, 16, 15, 19)
BLANK_FACE(MONO_21, 21, 20, 25)
BLANK_FACE(SERIF, 17, 16, 21)
BLANK_FACE(SERIF_30, 30, 28, 36)
BLANK_FACE(SERIF_BOLD, 17, 16, 21)
BLANK_FACE(SERIF_BOLD_30, 30, 29, 37)
BLANK_FACE(SERIF_ITALIC, 17, 16, 21)
BLANK_FACE(SERIF_BOLD_ITALIC, 17, 16, 21)
BLANK_FACE(UI_11, 11, 11, 14)
BLANK_FACE(UI_12, 12, 12, 15)
BLANK_FACE(UI_13, 13, 13, 17)
BLANK_FACE(UI_14, 14, 13, 17)
BLANK_FACE(UI_15, 15, 14, 18)
BLANK_FACE(UI_16, 16, 15, 19)
BLANK_FACE(UI_19, 19, 18, 23)
BLANK_FACE(UI_24, 24, 23, 29)
BLANK_FACE(UI_BOLD_13, 13, 13, 17)
BLANK_FACE(UI_BOLD_14, 14, 13, 17)
BLANK_FACE(UI_BOLD_15, 15, 14, 18)
BLANK_FACE(UI_BOLD_16, 16, 15, 19)
BLANK_FACE(UI_BOLD_19, 19, 18, 23)
BLANK_FACE(UI_BOLD_24, 24, 23, 29)
BLANK_FACE(UI_ITALIC_13, 13, 13, 17)
BLANK_FACE(UI_ITALIC_14, 14, 13, 17)
BLANK_FACE(UI_ITALIC_15, 15, 14, 18)
BLANK_FACE(UI_ITALIC_16, 16, 15, 19)
BLANK_FACE(SERIF_13, 13, 13, 17)
BLANK_FACE(SERIF_14, 14, 13, 17)
BLANK_FACE(SERIF_15, 15, 14, 18)
BLANK_FACE(SERIF_16, 16, 15, 19)
BLANK_FACE(SERIF_19, 19, 18, 23)
BLANK_FACE(SERIF_24, 24, 23, 29)
BLANK_FACE(SERIF_BOLD_14, 14, 14, 18)
BLANK_FACE(SERIF_BOLD_16, 16, 16, 20)
BLANK_FACE(SERIF_BOLD_19, 19, 18, 23)
BLANK_FACE(SERIF_BOLD_24, 24, 23, 29)
BLANK_FACE(SERIF_ITALIC_14, 14, 13, 17)
BLANK_FACE(SERIF_ITALIC_16, 16, 15, 19)
BLANK_FACE(MONO_11, 11, 11, 14)
BLANK_FACE(MONO_12, 12, 12, 15)
BLANK_FACE(MONO_13, 13, 13, 17)
BLANK_FACE(MONO_14, 14, 13, 17)
BLANK_FACE(MONO_15, 15, 14, 18)

#undef BLANK_FACE

}  // namespace aios::userland::fonts
