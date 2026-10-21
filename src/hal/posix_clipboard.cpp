// The hosted clipboard. SDL owns the window, so SDL's clipboard IS the host's:
// text copied here pastes into the host's other programs and back. Without
// SDL there is no window and no window system to share with, so the machine
// keeps a clipboard of its own and says as much.
#include "aios/hal/clipboard.hpp"
#include "aios/hal/sync.hpp"

#if defined(AIOS_HAS_SDL2)
#include <SDL.h>
#endif

namespace aios::hal {
namespace {
Mutex& own_mutex() { static Mutex m; return m; }
std::string& own_clipboard() { static std::string s; return s; }
}  // namespace

Result<std::string> clipboard_get() {
#if defined(AIOS_HAS_SDL2)
  if (SDL_WasInit(SDL_INIT_VIDEO) && SDL_HasClipboardText()) {
    char* text = SDL_GetClipboardText();
    if (text) {
      std::string out(text);
      SDL_free(text);
      return out;
    }
  }
#endif
  LockGuard l(own_mutex());
  return own_clipboard();
}

Result<void> clipboard_set(const std::string& text) {
#if defined(AIOS_HAS_SDL2)
  if (SDL_WasInit(SDL_INIT_VIDEO)) {
    if (SDL_SetClipboardText(text.c_str()) == 0) return Ok();
  }
#endif
  LockGuard l(own_mutex());
  own_clipboard() = text;
  return Ok();
}

}  // namespace aios::hal
