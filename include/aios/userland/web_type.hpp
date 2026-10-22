#pragma once
// Type: measuring and drawing text with the machine's own fonts (the atlas, and
// the outline engine for what the atlas does not carry), into an ARGB buffer.
// The web pane and faces both set text with it (web_type.cpp); nothing here
// needs a desktop or a browser.
#include <cstdint>
#include <string>
#include <vector>

#include "aios/userland/web.hpp"   // web::FontKey

namespace aios::userland {

// Text is measured and drawn from the atlas, which is fixed bitmaps cut at build
// time by FreeType and there is no font engine in ring 0. A document asks for
// 30px headings and 12px captions, so anything that is not the face's own size
// is resampled from its coverage — bilinearly, which keeps the antialiasing
// that made the desktop's text look like text.
int web_text_width(const std::string& text, const web::FontKey& f);
int web_line_height(const web::FontKey& f);
int web_ascent(const web::FontKey& f);

// A rectangle a draw is confined to, in buffer coordinates. `overflow: hidden`
// is a promise a page makes about where its content stops, and the painter used
// to ignore it entirely: layout computed a clip for every fragment inside such
// a box and nothing ever read it, so a long line ran straight over whatever was
// beside it.
struct DrawClip {
  int x0 = -(1 << 28), y0 = -(1 << 28), x1 = 1 << 28, y1 = 1 << 28;
  bool has(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};

// `text` at (x, y) - y is the top of the line - in `color`, clipped to the buffer and to `clip`.
void web_draw_text_px(std::vector<uint32_t>& buf, int bw, int bh, int x, int y, const std::string& text, uint32_t color,
                      const web::FontKey& f, const DrawClip& clip = DrawClip{});

// The ASCII stand-in for a code point the atlas cannot draw (curly quotes, dashes, bullets ...).
char ascii_for_cp(uint32_t cp);

// The characters in `text` this display cannot draw and has no stand-in for
// (they would come out as '?'), each once, space-separated. What a look at a
// surface reports back to the mind that wrote it.
std::string display_glyph_gaps(const std::string& text);

}  // namespace aios::userland
