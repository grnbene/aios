// Glyphs the atlas does not carry, rasterised while the machine is running.
//
// src/userland/font_atlas.cpp is cut at build time and stops where mkfont.py's
// code point list stops: ASCII, Latin-1, Latin Extended-A, and a hand-picked
// bag of punctuation. Everything else — Greek, Cyrillic, the arrows and stars
// past U+2600, anything a person types in their own alphabet — came off the
// screen as '?', because a baked atlas can only ever draw what someone
// enumerated before the build.
//
// This is the other half: a TrueType outline reader and a scanline rasteriser,
// so a code point the atlas misses is cut from the real typeface AT THAT
// MOMENT, cached, and drawn like any other glyph. The output is the same
// 8-bit coverage the atlas ships, on the same baseline, so the compositor
// blends it through exactly the same path.
#pragma once
#include <cstddef>
#include <cstdint>

#include "aios/userland/font_atlas.hpp"

namespace aios::userland::glyphs {

// One glyph, cut just now. `g.off` is always 0 — `cov` points straight at this
// glyph's own rows rather than into a face-wide blob, because a runtime glyph
// has no blob to be an offset into.
struct Rasterized {
  fonts::Glyph g;
  const uint8_t* cov;
};

// The glyph `face` would draw for `cp` if its atlas went that far, or nullptr
// when no installed typeface has the code point at all. Cached: the second ask
// for the same (face, cp) is a hash lookup.
const Rasterized* rasterize(const fonts::Face& face, uint32_t cp);

// Whether any outline font was found. False means every ask will miss and the
// caller's transliteration is still the last word.
bool available();

// Hand the engine a font's bytes directly. The hosted build finds its own on
// disk; bare metal has no directory to search, so the image passes what it
// embedded. Returns false if the bytes are not a TrueType outline font.
bool install(const uint8_t* bytes, size_t len, const char* name);

struct Stats {
  size_t fonts;    // outline files the engine holds
  size_t cached;   // glyphs cut so far
  size_t misses;   // asks no installed font could answer
  size_t cov_bytes;
};
Stats stats();

// The typefaces the engine ended up with, newline separated, for `info` and
// for anyone asking why a character still will not draw.
const char* installed_names();

}  // namespace aios::userland::glyphs
