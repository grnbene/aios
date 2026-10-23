// Panes as real windows on Windows, with no library: the Win32 API itself (user32 and gdi32, which are the
// operating system). The counterpart of display_x11.cpp, and the same contract:
//
// Each registered pane is a window; every frame its draw callback paints the pane's canvas and the canvas is
// blitted to the window (SetDIBitsToDevice, only when it changed). Input comes back to the pane as the event
// maps display_x11.cpp documents - mouse_down/mouse_up/click (a click follows a mouse_up over the window the
// button went down in), mouse_move, wheel, key/key_up (key names as SDL's, lower case), text, resize, close.
//
// Scripted input (run_panes({script: [...]})) goes to the pane one event a frame through the same code. With
// GRAIN_VIDEO=dummy (or SDL_VIDEODRIVER=dummy) no window is made: panes are drawn and scripted, not shown.
#include "display_win32.hpp"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace grain_cli {

namespace g = aios::userland::grain;

namespace {

// ---- event maps ------------------------------------------------------------

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
bool flag_field(const g::Value& ev, const char* name) {
  const g::Value* v = fields(ev).find(name);
  return v && v->truthy();
}

std::wstring wide(const std::string& s) {
  if (s.empty()) return L"";
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

void append_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) out += (char)cp;
  else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
  else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}

// The name a key event carries: what SDL called the key, lower-cased (as on X11).
std::string key_name(WPARAM vk, LPARAM lp) {
  const bool extended = (lp >> 24) & 1;
  if (vk >= 'A' && vk <= 'Z') return std::string(1, (char)(vk - 'A' + 'a'));
  if (vk >= '0' && vk <= '9') return std::string(1, (char)vk);
  if (vk >= VK_F1 && vk <= VK_F24) return "f" + std::to_string(vk - VK_F1 + 1);
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "keypad " + std::to_string(vk - VK_NUMPAD0);
  switch (vk) {
    case VK_BACK: return "backspace";
    case VK_TAB: return "tab";
    case VK_RETURN: return extended ? "keypad enter" : "return";
    case VK_ESCAPE: return "escape";
    case VK_SPACE: return "space";
    case VK_DELETE: return "delete";
    case VK_HOME: return "home";
    case VK_END: return "end";
    case VK_PRIOR: return "pageup";
    case VK_NEXT: return "pagedown";
    case VK_INSERT: return "insert";
    case VK_LEFT: return "left";
    case VK_RIGHT: return "right";
    case VK_UP: return "up";
    case VK_DOWN: return "down";
    case VK_SHIFT: return (MapVirtualKeyW((lp >> 16) & 0xFF, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT) ? "right shift" : "left shift";
    case VK_CONTROL: return extended ? "right ctrl" : "left ctrl";
    case VK_MENU: return extended ? "right alt" : "left alt";
    case VK_LWIN: return "left gui";
    case VK_RWIN: return "right gui";
    case VK_CAPITAL: return "capslock";
    case VK_NUMLOCK: return "numlock";
    case VK_SCROLL: return "scrolllock";
    case VK_PAUSE: return "pause";
    case VK_SNAPSHOT: return "printscreen";
    case VK_APPS: return "menu";
    case VK_MULTIPLY: return "keypad *";
    case VK_ADD: return "keypad +";
    case VK_SUBTRACT: return "keypad -";
    case VK_DECIMAL: return "keypad .";
    case VK_DIVIDE: return "keypad /";
    default: break;
  }
  // punctuation: the character the key types unshifted, in this keyboard's layout
  const UINT ch = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_CHAR) & 0x7FFF;
  if (ch >= 0x21) { std::string s; append_utf8(s, (uint32_t)towlower((wint_t)ch)); return s; }
  return "unknown";
}

// The key a script names, as a virtual key (the inverse of key_name for the names a script uses).
bool known_key(std::string name) {
  for (char& c : name) c = (char)std::tolower((unsigned char)c);
  static const char* named[] = {"space", "backspace", "tab", "return", "enter", "escape", "delete", "home", "left", "up", "right", "down",
                                "pageup", "pagedown", "end", "insert", "left shift", "right shift", "left ctrl", "right ctrl", "left alt",
                                "right alt", "left gui", "right gui", "capslock", "numlock", "keypad enter"};
  for (const char* n : named) if (name == n) return true;
  if (name.size() == 1 && (unsigned char)name[0] > 0x20 && (unsigned char)name[0] < 0x7f) return true;
  if (name.size() >= 2 && name[0] == 'f') { const int n = std::atoi(name.c_str() + 1); return n >= 1 && n <= 24; }
  return false;
}
std::string canonical_key(std::string name) {
  for (char& c : name) c = (char)std::tolower((unsigned char)c);
  return name == "enter" ? "return" : name;
}

struct Window {
  HWND hwnd = nullptr;
  int width = 0, height = 0;          // the canvas, as last drawn
  std::vector<uint32_t> last;         // what the window shows now
  bool force = true;
  bool button_down[4] = {false, false, false, false};
  wchar_t high_surrogate = 0;         // the first half of a character outside the BMP (WM_CHAR sends two)
};

class Win32Display final : public g::Display {
 public:
  bool run(g::Vm& vm, const g::DisplayRun& opts, std::string& error) override {
    const char* v = std::getenv("GRAIN_VIDEO");
    const char* s = std::getenv("SDL_VIDEODRIVER");
    headless_ = (v && std::string(v) == "dummy") || (s && std::string(s) == "dummy");
    vm_ = &vm;
    if (!headless_ && !register_class(error)) return false;
    const bool ok = loop(vm, opts, error);
    for (auto& [id, w] : windows_) destroy(w);
    windows_.clear();
    vm_ = nullptr;
    return ok;
  }

 private:
  bool headless_ = false;
  g::Vm* vm_ = nullptr;
  std::map<uint64_t, Window> windows_;
  std::string pending_;   // an error from inside the window procedure, raised by the loop
  static constexpr const wchar_t* kClass = L"GrainPane";

  static Win32Display* self_;

  bool register_class(std::string& error) {
    static bool done = false;
    if (done) return true;
    WNDCLASSEXW wc;
    std::memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = &Win32Display::proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { error = "run_panes(): cannot register the window class"; return false; }
    done = true;
    return true;
  }

  void destroy(Window& w) {
    if (w.hwnd) { SetWindowLongPtrW(w.hwnd, GWLP_USERDATA, 0); DestroyWindow(w.hwnd); }
    w.hwnd = nullptr;
  }

  uint64_t pane_of(HWND h) const {
    for (const auto& [id, w] : windows_) if (w.hwnd == h) return id;
    return 0;
  }

  bool create(const g::PaneInfo& p, Window& w, std::string& error) {
    w.width = p.width;
    w.height = p.height;
    if (headless_) return true;
    RECT r{0, 0, p.width, p.height};
    const DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRectEx(&r, style, FALSE, 0);   // the size asked for is the drawing area, not the frame
    w.hwnd = CreateWindowExW(0, kClass, wide(p.title).c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!w.hwnd) { error = "run_panes(): cannot open a window for \"" + p.title + "\""; return false; }
    ShowWindow(w.hwnd, SW_SHOWNORMAL);
    UpdateWindow(w.hwnd);
    return true;
  }

  bool sync(const g::Vm& vm, std::string& error) {
    const auto panes = g::pane_list(vm);
    for (auto it = windows_.begin(); it != windows_.end();) {
      const bool alive = std::any_of(panes.begin(), panes.end(), [&](const g::PaneInfo& p) { return p.id == it->first; });
      if (alive) { ++it; continue; }
      destroy(it->second);
      it = windows_.erase(it);
    }
    for (const auto& p : panes) {
      if (windows_.count(p.id)) continue;
      Window w;
      if (!create(p, w, error)) return false;
      windows_[p.id] = std::move(w);
    }
    return true;
  }

  // ---- input ----------------------------------------------------------------

  g::Value key_event(const char* type, uint64_t pane, const std::string& key, bool shift, bool ctrl, bool alt, bool meta) {
    g::Value ev = event_map(type, pane);
    fields(ev).put("key", g::Value::string(key));
    fields(ev).put("shift", g::Value::boolean(shift));
    fields(ev).put("ctrl", g::Value::boolean(ctrl));
    fields(ev).put("alt", g::Value::boolean(alt));
    fields(ev).put("meta", g::Value::boolean(meta));
    return ev;
  }

  bool mouse_button(uint64_t pane, bool down, int x, int y, const std::string& button, std::string& error) {
    g::Value ev = event_map(down ? "mouse_down" : "mouse_up", pane);
    fields(ev).put("x", g::Value::integer(x));
    fields(ev).put("y", g::Value::integer(y));
    fields(ev).put("button", g::Value::string(button));
    if (!g::pane_event(*vm_, pane, ev, error)) return false;
    auto it = windows_.find(pane);
    if (it == windows_.end()) return true;
    const int b = button == "left" ? 1 : button == "middle" ? 2 : button == "right" ? 3 : 0;
    if (down) { it->second.button_down[b] = true; return true; }
    if (!it->second.button_down[b]) return true;
    it->second.button_down[b] = false;
    g::Value click = event_map("click", pane);
    fields(click).put("x", g::Value::integer(x));
    fields(click).put("y", g::Value::integer(y));
    fields(click).put("button", g::Value::string(button));
    return g::pane_event(*vm_, pane, click, error);
  }

  bool wheel(uint64_t pane, double dx, double dy, std::string& error) {
    g::Value ev = event_map("wheel", pane);
    fields(ev).put("dx", g::Value::real(dx));
    fields(ev).put("dy", g::Value::real(dy));
    return g::pane_event(*vm_, pane, ev, error);
  }

  bool text(uint64_t pane, const std::string& t, std::string& error) {
    g::Value ev = event_map("text", pane);
    fields(ev).put("text", g::Value::string(t));
    return g::pane_event(*vm_, pane, ev, error);
  }

  static bool down(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

  // One message for a pane's window; false (with the error kept) when the program's callback failed.
  bool handle(HWND h, UINT msg, WPARAM wp, LPARAM lp, std::string& error) {
    const uint64_t pane = pane_of(h);
    if (!pane) return true;
    switch (msg) {
      case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONDOWN:
        SetCapture(h);
        return mouse_button(pane, true, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), msg == WM_LBUTTONDOWN ? "left" : msg == WM_MBUTTONDOWN ? "middle" : "right", error);
      case WM_LBUTTONUP: case WM_MBUTTONUP: case WM_RBUTTONUP:
        ReleaseCapture();
        return mouse_button(pane, false, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), msg == WM_LBUTTONUP ? "left" : msg == WM_MBUTTONUP ? "middle" : "right", error);
      case WM_MOUSEMOVE: {
        g::Value ev = event_map("mouse_move", pane);
        fields(ev).put("x", g::Value::integer(GET_X_LPARAM(lp)));
        fields(ev).put("y", g::Value::integer(GET_Y_LPARAM(lp)));
        return g::pane_event(*vm_, pane, ev, error);
      }
      case WM_MOUSEWHEEL: return wheel(pane, 0, (double)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA, error);
      case WM_MOUSEHWHEEL: return wheel(pane, (double)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA, 0, error);
      case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        const bool press = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        return g::pane_event(*vm_, pane, key_event(press ? "key" : "key_up", pane, key_name(wp, lp), down(VK_SHIFT), down(VK_CONTROL), down(VK_MENU),
                                                   down(VK_LWIN) || down(VK_RWIN)), error);
      }
      case WM_CHAR: {
        const wchar_t c = (wchar_t)wp;
        auto it = windows_.find(pane);
        if (c >= 0xD800 && c <= 0xDBFF) { if (it != windows_.end()) it->second.high_surrogate = c; return true; }
        uint32_t cp = c;
        if (c >= 0xDC00 && c <= 0xDFFF) {
          if (it == windows_.end() || !it->second.high_surrogate) return true;
          cp = 0x10000 + (((uint32_t)it->second.high_surrogate - 0xD800) << 10) + ((uint32_t)c - 0xDC00);
          it->second.high_surrogate = 0;
        }
        if (cp < 0x20 || cp == 0x7F) return true;      // Enter, Tab, Backspace and control keys arrive as key events
        if (down(VK_CONTROL) && !down(VK_MENU)) return true;   // Ctrl+letter is a key, not text (AltGr is Ctrl+Alt: text)
        std::string t;
        append_utf8(t, cp);
        return text(pane, t, error);
      }
      case WM_SIZE: {
        const int w = LOWORD(lp), hh = HIWORD(lp);
        auto it = windows_.find(pane);
        if (it == windows_.end() || w < 1 || hh < 1 || (w == it->second.width && hh == it->second.height)) return true;
        it->second.force = true;
        return g::pane_resized(*vm_, pane, w, hh, error);
      }
      case WM_CLOSE: return g::pane_closed(*vm_, pane, error);
      default: return true;
    }
  }

  static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Win32Display* d = self_;
    switch (msg) {
      case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        if (d) { const uint64_t pane = d->pane_of(h); auto it = d->windows_.find(pane); if (it != d->windows_.end()) it->second.force = true; }
        return 0;
      }
      case WM_ERASEBKGND: return 1;   // the frame covers it: no flash of white
      case WM_CLOSE:
      case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONDOWN: case WM_LBUTTONUP: case WM_MBUTTONUP: case WM_RBUTTONUP:
      case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
      case WM_CHAR: case WM_SIZE:
        if (d && d->vm_ && d->pending_.empty()) {
          std::string err;
          if (!d->handle(h, msg, wp, lp, err)) d->pending_ = err;
        }
        if (msg == WM_CLOSE) return 0;   // the pane decides; its window goes when the pane does
        if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) break;   // Alt+F4 and the menu keys keep working
        return 0;
      default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
  }

  // A scripted event, to the pane as the window system would deliver it.
  bool scripted(const g::Value& ev, std::string& error) {
    const std::string type = str_field(ev, "type");
    const uint64_t pane = static_cast<uint64_t>(int_field(ev, "pane"));
    auto it = windows_.find(pane);
    if (it == windows_.end()) { error = "run_panes(): a scripted \"" + type + "\" names pane " + std::to_string(pane) + ", which has no window"; return false; }
    const int x = static_cast<int>(int_field(ev, "x")), y = static_cast<int>(int_field(ev, "y"));
    if (type == "mouse_down" || type == "mouse_up") return mouse_button(pane, type == "mouse_down", x, y, str_field(ev, "button").empty() ? "left" : str_field(ev, "button"), error);
    if (type == "mouse_move") {
      g::Value m = event_map("mouse_move", pane);
      fields(m).put("x", g::Value::integer(x));
      fields(m).put("y", g::Value::integer(y));
      return g::pane_event(*vm_, pane, m, error);
    }
    if (type == "wheel") {
      const g::Value* dx = fields(ev).find("dx");
      const g::Value* dy = fields(ev).find("dy");
      return wheel(pane, dx && dx->is_number() ? dx->as_double() : 0, dy && dy->is_number() ? dy->as_double() : 0, error);
    }
    if (type == "key" || type == "key_up") {
      const std::string name = str_field(ev, "key");
      if (!known_key(name)) { error = "run_panes(): a scripted key \"" + name + "\" is not a key grain knows"; return false; }
      return g::pane_event(*vm_, pane, key_event(type.c_str(), pane, canonical_key(name), flag_field(ev, "shift"), flag_field(ev, "ctrl"), flag_field(ev, "alt"), flag_field(ev, "meta")), error);
    }
    if (type == "text") return text(pane, str_field(ev, "text"), error);
    if (type == "resize") {
      const int w = std::max(1, (int)int_field(ev, "width", 1)), h = std::max(1, (int)int_field(ev, "height", 1));
      if (headless_ || !it->second.hwnd) { it->second.force = true; return g::pane_resized(*vm_, pane, w, h, error); }
      RECT r{0, 0, w, h};
      AdjustWindowRectEx(&r, (DWORD)GetWindowLongW(it->second.hwnd, GWL_STYLE), FALSE, 0);
      SetWindowPos(it->second.hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);   // WM_SIZE brings it back
      return true;
    }
    if (type == "close") return g::pane_closed(*vm_, pane, error);
    error = "run_panes(): a scripted event of type \"" + type + "\" is not one a window sends";
    return false;
  }

  // ---- drawing --------------------------------------------------------------

  bool draw(uint64_t id, Window& w, std::string& error) {
    std::vector<uint32_t> px;
    int pw = 0, ph = 0;
    if (!g::pane_frame(*vm_, id, px, pw, ph, error)) return false;
    if (pw < 1 || ph < 1) return true;
    if (headless_ || !w.hwnd) { w.width = pw; w.height = ph; return true; }
    const bool same = !w.force && pw == w.width && ph == w.height && w.last.size() == px.size() &&
                      std::memcmp(w.last.data(), px.data(), px.size() * sizeof(uint32_t)) == 0;
    w.width = pw;
    w.height = ph;
    if (same) return true;
    BITMAPINFO bi;
    std::memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = pw;
    bi.bmiHeader.biHeight = -ph;   // top-down rows, as the canvas holds them
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;  // 0xAARRGGBB little-endian is the BGRA Windows reads
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(w.hwnd);
    SetDIBitsToDevice(dc, 0, 0, (DWORD)pw, (DWORD)ph, 0, 0, 0, (UINT)ph, px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(w.hwnd, dc);
    w.last = std::move(px);
    w.force = false;
    return true;
  }

  bool loop(g::Vm& vm, const g::DisplayRun& opts, std::string& error) {
    using clock = std::chrono::steady_clock;
    constexpr auto kFrame = std::chrono::milliseconds(16);
    self_ = this;
    struct Clear { ~Clear() { self_ = nullptr; } } clear;
    size_t next_script = 0;
    for (int64_t frame = 0; opts.max_frames <= 0 || frame < opts.max_frames; ++frame) {
      const auto started = clock::now();
      if (!sync(vm, error)) return false;
      if (windows_.empty()) return true;   // every pane closed: run_panes() is done
      if (next_script < opts.script.size() && !scripted(opts.script[next_script++], error)) return false;
      if (!headless_) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
          TranslateMessage(&msg);   // WM_KEYDOWN -> WM_CHAR, in this keyboard's layout
          DispatchMessageW(&msg);
          if (!pending_.empty()) { error = "run_panes(): " + pending_; pending_.clear(); return false; }
        }
      }
      if (!sync(vm, error)) return false;
      { std::string timer_error; g::run_timers(vm, timer_error); if (!timer_error.empty()) { error = "run_panes(): " + timer_error; return false; } }
      for (auto& [id, w] : windows_)
        if (!draw(id, w, error)) return false;
      const auto spent = clock::now() - started;
      if (spent < kFrame) {
        const DWORD ms = (DWORD)std::chrono::duration_cast<std::chrono::milliseconds>(kFrame - spent).count();
        if (headless_) std::this_thread::sleep_for(kFrame - spent);
        else MsgWaitForMultipleObjects(0, nullptr, FALSE, ms, QS_ALLINPUT);   // wake on input, keep frames at 60
      }
    }
    return true;
  }
};

Win32Display* Win32Display::self_ = nullptr;

}  // namespace

g::Display& win32_display() {
  static Win32Display d;
  return d;
}

}  // namespace grain_cli

#endif  // _WIN32
