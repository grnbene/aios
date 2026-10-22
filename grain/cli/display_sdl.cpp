// Panes as real windows, through SDL2: what run_panes() shows on in the
// standalone grain, on macOS, Linux (X11 or Wayland) and Windows.
//
// Each registered pane is a window; every frame its draw callback paints the
// pane's canvas and the canvas is copied to the window. Input comes back to
// the pane as event maps:
//   {type: "mouse_down" | "mouse_up" | "click", pane, x, y, button: "left" | "middle" | "right"}
//   {type: "mouse_move", pane, x, y}
//   {type: "wheel", pane, dx, dy}   (reals: a trackpad scrolls in fractions; up and right are positive)
//   {type: "key" | "key_up", pane, key: "a" | "return" | "left" | ..., shift, ctrl, alt, meta}
//   {type: "text", pane, text}
//   {type: "resize", width, height} and {type: "close"} (from grain_builtins.cpp)
// A "click" follows a mouse_up over the window the button went down in.
//
// Scripted input (run_panes({script: [...]})) is pushed into SDL's own queue,
// one event a frame, so it goes through the same translation as a person's;
// with SDL_VIDEODRIVER=dummy that is how the display is tested without a screen.
#include "display_sdl.hpp"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace grain_cli {

namespace g = aios::userland::grain;

namespace {

struct Window {
  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  SDL_Texture* texture = nullptr;
  int tex_w = 0, tex_h = 0;
  uint32_t sdl_id = 0;
  bool button_down[4] = {false, false, false, false};
};

const char* button_name(uint8_t b) {
  switch (b) {
    case SDL_BUTTON_LEFT: return "left";
    case SDL_BUTTON_MIDDLE: return "middle";
    case SDL_BUTTON_RIGHT: return "right";
    default: return "other";
  }
}

uint8_t button_of(const std::string& name) {
  if (name == "middle") return SDL_BUTTON_MIDDLE;
  if (name == "right") return SDL_BUTTON_RIGHT;
  return SDL_BUTTON_LEFT;
}

std::string key_name(SDL_Keycode k) {
  std::string n = SDL_GetKeyName(k);
  for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return n;
}

g::Value event_map(const char* type, uint64_t pane) {
  auto m = std::make_shared<g::MapObj>();
  m->put("type", g::Value::string(type));
  m->put("pane", g::Value::integer(static_cast<int64_t>(pane)));
  return g::Value::object(g::Type::Map, m);
}

g::MapObj& fields(const g::Value& v) { return *static_cast<g::MapObj*>(v.obj.get()); }

int64_t int_field(const g::Value& ev, const char* name, int64_t dflt = 0) {
  const g::Value* v = fields(ev).find(name);
  return v && v->is_number() ? v->as_int() : dflt;
}

std::string str_field(const g::Value& ev, const char* name) {
  const g::Value* v = fields(ev).find(name);
  return v && v->type == g::Type::Str ? v->str() : std::string();
}

class SdlDisplay final : public g::Display {
 public:
  bool run(g::Vm& vm, const g::DisplayRun& opts, std::string& error) override {
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");   // a canvas drawn at 1x, shown on a 2x screen, scaled smoothly
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { error = std::string("run_panes(): cannot open the display: ") + SDL_GetError(); return false; }
    SDL_StartTextInput();
    bool ok = loop(vm, opts, error);
    for (auto& [id, w] : windows_) destroy(w);
    windows_.clear();
    SDL_StopTextInput();
    SDL_Quit();
    return ok;
  }

 private:
  std::map<uint64_t, Window> windows_;

  static void destroy(Window& w) {
    if (w.texture) SDL_DestroyTexture(w.texture);
    if (w.renderer) SDL_DestroyRenderer(w.renderer);
    if (w.window) SDL_DestroyWindow(w.window);
    w = Window{};
  }

  uint64_t pane_of(uint32_t sdl_id) const {
    for (const auto& [id, w] : windows_)
      if (w.sdl_id == sdl_id) return id;
    return 0;
  }

  // Windows for new panes; none for panes the program closed.
  bool sync(std::string& error) {
    const auto panes = g::pane_list();
    for (auto it = windows_.begin(); it != windows_.end();) {
      const bool alive = std::any_of(panes.begin(), panes.end(), [&](const g::PaneInfo& p) { return p.id == it->first; });
      if (alive) { ++it; continue; }
      destroy(it->second);
      it = windows_.erase(it);
    }
    for (const auto& p : panes) {
      if (windows_.count(p.id)) continue;
      Window w;
      w.window = SDL_CreateWindow(p.title.c_str(), SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, p.width, p.height,
                                  SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
      if (!w.window) { error = std::string("run_panes(): cannot open a window for \"") + p.title + "\": " + SDL_GetError(); return false; }
      w.renderer = SDL_CreateRenderer(w.window, -1, SDL_RENDERER_ACCELERATED);
      if (!w.renderer) w.renderer = SDL_CreateRenderer(w.window, -1, SDL_RENDERER_SOFTWARE);
      if (!w.renderer) { error = std::string("run_panes(): no renderer for \"") + p.title + "\": " + SDL_GetError(); destroy(w); return false; }
      w.sdl_id = SDL_GetWindowID(w.window);
      SDL_RenderSetLogicalSize(w.renderer, p.width, p.height);   // from the first event on (see draw)
      windows_[p.id] = w;
    }
    return true;
  }

  // A scripted event, into SDL's queue as the window system would send it.
  // Typed text is the exception: it goes straight to the pane, as its
  // translation is the text itself. sdl2-compat (Homebrew's SDL2, on SDL3)
  // crashes pushing an SDL_TEXTINPUT - SDL3 keeps the text as a pointer the
  // compat layer's conversion leaves null.
  bool push_scripted(g::Vm& vm, const g::Value& ev, std::string& error) {
    const std::string type = str_field(ev, "type");
    const uint64_t pane = static_cast<uint64_t>(int_field(ev, "pane"));
    auto it = windows_.find(pane);
    if (it == windows_.end()) { error = "run_panes(): a scripted \"" + type + "\" names pane " + std::to_string(pane) + ", which has no window"; return false; }
    Window& w = it->second;
    SDL_Event e;
    std::memset(&e, 0, sizeof e);
    if (type == "mouse_down" || type == "mouse_up") {
      e.type = type == "mouse_down" ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
      e.button.windowID = w.sdl_id;
      e.button.button = button_of(str_field(ev, "button"));
      e.button.state = type == "mouse_down" ? SDL_PRESSED : SDL_RELEASED;
      e.button.clicks = 1;
      e.button.x = static_cast<int>(int_field(ev, "x"));
      e.button.y = static_cast<int>(int_field(ev, "y"));
    } else if (type == "mouse_move") {
      e.type = SDL_MOUSEMOTION;
      e.motion.windowID = w.sdl_id;
      e.motion.x = static_cast<int>(int_field(ev, "x"));
      e.motion.y = static_cast<int>(int_field(ev, "y"));
    } else if (type == "wheel") {
      e.type = SDL_MOUSEWHEEL;
      e.wheel.windowID = w.sdl_id;
      e.wheel.x = static_cast<int>(int_field(ev, "dx"));
      e.wheel.y = static_cast<int>(int_field(ev, "dy"));
      e.wheel.preciseX = static_cast<float>(e.wheel.x);   // what SDL3 (and sdl2-compat on it) reads
      e.wheel.preciseY = static_cast<float>(e.wheel.y);
      e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    } else if (type == "key" || type == "key_up") {
      const SDL_Keycode k = SDL_GetKeyFromName(str_field(ev, "key").c_str());
      if (k == SDLK_UNKNOWN) { error = "run_panes(): a scripted key \"" + str_field(ev, "key") + "\" is not a key SDL knows"; return false; }
      e.type = type == "key" ? SDL_KEYDOWN : SDL_KEYUP;
      e.key.windowID = w.sdl_id;
      e.key.state = type == "key" ? SDL_PRESSED : SDL_RELEASED;
      e.key.keysym.sym = k;
      e.key.keysym.scancode = SDL_GetScancodeFromKey(k);
      uint16_t mod = 0;
      auto flag = [&](const char* name) { const g::Value* v = fields(ev).find(name); return v && v->truthy(); };
      if (flag("shift")) mod |= KMOD_LSHIFT;
      if (flag("ctrl")) mod |= KMOD_LCTRL;
      if (flag("alt")) mod |= KMOD_LALT;
      if (flag("meta")) mod |= KMOD_LGUI;
      e.key.keysym.mod = mod;
    } else if (type == "text") {
      g::Value t = event_map("text", pane);
      fields(t).put("text", g::Value::string(str_field(ev, "text")));
      return g::pane_event(vm, pane, t, error);
    } else if (type == "resize") {
      // Through the window system itself: the size change comes back as SDL's own event.
      SDL_SetWindowSize(w.window, static_cast<int>(int_field(ev, "width", 1)), static_cast<int>(int_field(ev, "height", 1)));
      return true;
    } else if (type == "close") {
      e.type = SDL_WINDOWEVENT;
      e.window.windowID = w.sdl_id;
      e.window.event = SDL_WINDOWEVENT_CLOSE;
    } else {
      error = "run_panes(): a scripted event of type \"" + type + "\" is not one a window sends";
      return false;
    }
    if (SDL_PushEvent(&e) < 0) { error = std::string("run_panes(): ") + SDL_GetError(); return false; }
    return true;
  }

  // One SDL event to the pane it belongs to.
  bool translate(g::Vm& vm, const SDL_Event& e, std::string& error) {
    switch (e.type) {
      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP: {
        const uint64_t pane = pane_of(e.button.windowID);
        if (!pane) return true;
        const bool down = e.type == SDL_MOUSEBUTTONDOWN;
        g::Value ev = event_map(down ? "mouse_down" : "mouse_up", pane);
        fields(ev).put("x", g::Value::integer(e.button.x));
        fields(ev).put("y", g::Value::integer(e.button.y));
        fields(ev).put("button", g::Value::string(button_name(e.button.button)));
        if (!g::pane_event(vm, pane, ev, error)) return false;
        const int b = e.button.button < 4 ? e.button.button : 0;
        auto it = windows_.find(pane);
        if (it == windows_.end()) return true;
        if (down) { it->second.button_down[b] = true; return true; }
        if (!it->second.button_down[b]) return true;
        it->second.button_down[b] = false;
        g::Value click = event_map("click", pane);
        fields(click).put("x", g::Value::integer(e.button.x));
        fields(click).put("y", g::Value::integer(e.button.y));
        fields(click).put("button", g::Value::string(button_name(e.button.button)));
        return g::pane_event(vm, pane, click, error);
      }
      case SDL_MOUSEMOTION: {
        const uint64_t pane = pane_of(e.motion.windowID);
        if (!pane) return true;
        g::Value ev = event_map("mouse_move", pane);
        fields(ev).put("x", g::Value::integer(e.motion.x));
        fields(ev).put("y", g::Value::integer(e.motion.y));
        return g::pane_event(vm, pane, ev, error);
      }
      case SDL_MOUSEWHEEL: {
        const uint64_t pane = pane_of(e.wheel.windowID);
        if (!pane) return true;
        g::Value ev = event_map("wheel", pane);
        // The float amounts: a trackpad scrolls in fractions, and SDL3 (under
        // sdl2-compat) keeps only those - its whole-step fields can read 0.
        const float flip = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
        fields(ev).put("dx", g::Value::real(static_cast<double>(e.wheel.preciseX * flip)));
        fields(ev).put("dy", g::Value::real(static_cast<double>(e.wheel.preciseY * flip)));
        return g::pane_event(vm, pane, ev, error);
      }
      case SDL_KEYDOWN:
      case SDL_KEYUP: {
        const uint64_t pane = pane_of(e.key.windowID);
        if (!pane) return true;
        g::Value ev = event_map(e.type == SDL_KEYDOWN ? "key" : "key_up", pane);
        fields(ev).put("key", g::Value::string(key_name(e.key.keysym.sym)));
        const uint16_t m = e.key.keysym.mod;
        fields(ev).put("shift", g::Value::boolean((m & KMOD_SHIFT) != 0));
        fields(ev).put("ctrl", g::Value::boolean((m & KMOD_CTRL) != 0));
        fields(ev).put("alt", g::Value::boolean((m & KMOD_ALT) != 0));
        fields(ev).put("meta", g::Value::boolean((m & KMOD_GUI) != 0));
        return g::pane_event(vm, pane, ev, error);
      }
      case SDL_TEXTINPUT: {
        const uint64_t pane = pane_of(e.text.windowID);
        if (!pane) return true;
        g::Value ev = event_map("text", pane);
        fields(ev).put("text", g::Value::string(e.text.text));
        return g::pane_event(vm, pane, ev, error);
      }
      case SDL_WINDOWEVENT: {
        const uint64_t pane = pane_of(e.window.windowID);
        if (!pane) return true;
        if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
          return g::pane_resized(vm, pane, e.window.data1, e.window.data2, error);
        if (e.window.event == SDL_WINDOWEVENT_CLOSE) return g::pane_closed(vm, pane, error);
        return true;
      }
      case SDL_QUIT: {
        // The application was asked to quit (Cmd-Q, the last window on some systems): every pane closes.
        for (const auto& p : g::pane_list())
          if (!g::pane_closed(vm, p.id, error)) return false;
        return true;
      }
      default:
        return true;
    }
  }

  bool draw(g::Vm& vm, uint64_t id, Window& w, std::string& error) {
    std::vector<uint32_t> px;
    int pw = 0, ph = 0;
    if (!g::pane_frame(vm, id, px, pw, ph, error)) return false;
    if (pw < 1 || ph < 1) return true;
    if (!w.texture || w.tex_w != pw || w.tex_h != ph) {
      if (w.texture) SDL_DestroyTexture(w.texture);
      w.texture = SDL_CreateTexture(w.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, pw, ph);
      if (!w.texture) { error = std::string("run_panes(): ") + SDL_GetError(); return false; }
      w.tex_w = pw;
      w.tex_h = ph;
      // The renderer works in the pane's own size: the canvas is scaled to the
      // window, and mouse positions come back in the pane's coordinates. On a
      // 2x screen SDL reports the mouse in pixels, so without this every click
      // landed at twice its place.
      SDL_RenderSetLogicalSize(w.renderer, pw, ph);
    }
    SDL_UpdateTexture(w.texture, nullptr, px.data(), pw * static_cast<int>(sizeof(uint32_t)));
    SDL_SetRenderDrawColor(w.renderer, 0, 0, 0, 255);
    SDL_RenderClear(w.renderer);
    SDL_RenderCopy(w.renderer, w.texture, nullptr, nullptr);
    SDL_RenderPresent(w.renderer);
    return true;
  }

  bool loop(g::Vm& vm, const g::DisplayRun& opts, std::string& error) {
    constexpr uint32_t kFrameMs = 16;   // about 60 frames a second
    size_t scripted = 0;
    for (int64_t frame = 0; opts.max_frames <= 0 || frame < opts.max_frames; ++frame) {
      const uint32_t started = SDL_GetTicks();
      if (!sync(error)) return false;
      if (windows_.empty()) return true;   // every pane closed: run_panes() is done
      if (scripted < opts.script.size() && !push_scripted(vm, opts.script[scripted++], error)) return false;
      SDL_Event e;
      while (SDL_PollEvent(&e))
        if (!translate(vm, e, error)) return false;
      if (!sync(error)) return false;
      for (auto& [id, w] : windows_)
        if (!draw(vm, id, w, error)) return false;
      const uint32_t spent = SDL_GetTicks() - started;
      if (spent < kFrameMs) SDL_Delay(kFrameMs - spent);
    }
    return true;
  }
};

}  // namespace

g::Display& sdl_display() {
  static SdlDisplay d;
  return d;
}

}  // namespace grain_cli
