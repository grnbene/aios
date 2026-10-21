#pragma once
// Editor colour themes.
//
// The grammar (source_lang) says what a run of source IS; the compositor owns
// the ink. This is the compositor's palette, but data-driven: a theme is a
// lookup from TokenClass to an ARGB colour plus a ground, and the active theme
// can be switched at runtime so the editor reads the same file in Panic Dark,
// Nord, Solarized or any of the others without touching the grammar or the
// render path.
//
// The palette's types must stay identical to what the render path already
// uses -- a uint32_t ARGB. Nothing downstream ever learns that a theme exists.
#include <cstdint>
#include <string>

#include "aios/userland/source_lang.hpp"

namespace aios::userland {

// A named set of inks, one per token class plus the editor's own ground.
// Eighteen colours, full coverage of every TokenClass. `ground` is the canvas
// every ink is chosen against so that switching themes carries the correct
// canvas with it (Panic Light's inks are illegible on Panic Dark's ground).
struct EditorTheme {
  std::string name;
  uint32_t ground;   // editor background
  uint32_t ink[18];  // indexed by (int)TokenClass; Text lives at slot 0
};

// The built-in themes, in display order. Panic Dark is first and is the
// default, preserved verbatim from the original single hardcoded palette so
// that nothing that already reads as itself changes.
const EditorTheme* the_themes(size_t* out_count);

// The currently active theme. Starts as Panic Dark. Not thread-safe; the
// editor calls set_active_theme from its own thread only.
const EditorTheme& active_theme();

// Switch themes by name ("boba", "panic-boba", "panic-dark", "panic-light", "nord", "solarized",
// "monokai"). Returns true if a theme by that name exists and was applied.
bool set_active_theme(const std::string& name);

// The ink a token class is drawn in under the active theme. This is the only
// function the render path needs to call; it replaces the old switch that was
// hardwired to one palette.
inline uint32_t colour_for(TokenClass cls) {
  return active_theme().ink[(int)cls];
}

}  // namespace aios::userland
