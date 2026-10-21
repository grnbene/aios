// aios-web-shot: the browser pane as a service. It runs the same pane the desktop opens on F6 (the engine's fragments
// painted by the compositor), offscreen, and answers one command per line on stdin with one JSON line on stdout, so
// another program can show the page as the engine paints it and drive it with clicks, wheel and keys.
//
//   aios-web-shot <root> <view-width> <view-height>
//
//   go <url>          fetch, lay out, paint
//   click <x> <y>     a click at a point of the picture last written (page viewport coordinates)
//   move <x> <y>      the pointer, so hover and the wheel know where they are
//   wheel <notches>   + scrolls up, - down, over the point last moved to
//   key <name>        enter | backspace | delete | escape | tab | up | down | left | right | pageup | pagedown | home | end | ctrla | ctrlc | ctrlv | ctrlx | ctrll
//   type <text>       text into whatever has the keyboard
//   back | forward | reload
//   size <w> <h>      the page viewport the next picture covers
//   shot <png path>   write the page viewport as a PNG; the reply says where the page is and where its links are
//   quit
//
// Every reply is {"ok":true|false, ...}. A shot's reply: url, title, view {w,h}, content_height, scroll, can_back,
// can_forward, message, links [{x,y,w,h,href,text}] (the boxes on screen now, in picture coordinates).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "aios/hal/keys.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/userland/compositor.hpp"
#include "aios/userland/web.hpp"
#include "aios/userland/web_pane.hpp"

using namespace aios;

namespace {

constexpr int kChromeH = 34;   // the pane's own address row, not drawn into the picture
constexpr int kStatusH = 20;   // its status strip, likewise

std::string json_str(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; } else o += (char)c;
    }
  }
  return o + "\"";
}

void reply(const std::string& body) { std::cout << "{" << body << "}\n" << std::flush; }
void fail(const std::string& why) { reply("\"ok\":false,\"error\":" + json_str(why)); }

struct Desk {
  std::unique_ptr<userland::HardwareCompositor> c;
  uint32_t dw = 0, dh = 0;
  int view_w = 0, view_h = 0;
  Kernel* kernel = nullptr;
  Surface* console = nullptr;
  int px = 0, py = 0;   // the pointer, in picture coordinates

  userland::WindowSurface* pane() {
    userland::WindowSurface* w = c ? c->focused_window() : nullptr;
    return (w && w->type == userland::WindowType::Browser) ? w : nullptr;
  }
  void frame(int n = 1, int ms = 30) {
    for (int i = 0; i < n; ++i) { c->render_desktop(*kernel); if (ms) std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
  }
  // The pane's content origin on the desktop (frame border and title bar), and the page area below the chrome.
  void origin(int* x, int* y) {
    userland::WindowSurface* w = pane();
    *x = w ? w->bounds.x + 1 : 0;
    *y = w ? w->bounds.y + 26 + kChromeH : 0;
  }
  // Wait until the page has stopped working: a click or a load runs on the pane's own worker.
  std::string last_working;
  bool settle(int max_ms = 60000) {
    // A frame lays the page out under the lock the page's own worker needs, so painting back to back starves the very work
    // being waited on (a big page's layout takes longer than a frame). One frame to let the input land, then wait without
    // painting until the worker is done, then paint the result.
    auto t0 = std::chrono::steady_clock::now();
    auto expired = [&] { return std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(max_ms); };
    frame(1, 0);
    for (int round = 0; round < 6; ++round) {
      int idle = 0;
      while (idle < 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        userland::WindowSurface* w = pane();
        const bool busy = w && w->web && (w->web->worker->busy() || !w->web->working.empty());
        if (w && w->web) last_working = w->web->working;
        idle = busy ? 0 : idle + 1;
        if (expired()) return false;
      }
      frame(2, 0);   // paint what the work produced; it may start more (pictures)
      userland::WindowSurface* w = pane();
      if (!(w && w->web && (w->web->worker->busy() || !w->web->working.empty()))) return true;
    }
    return true;
  }
  bool up(int vw, int vh) {
    view_w = vw; view_h = vh;
    // The desktop's own furniture (top bar, dock, window frame) is constant, so size the desktop from what a maximized
    // window leaves: measure once with a trial desktop, then build the real one so the page viewport is exactly vw x vh.
    auto make = [&](uint32_t w, uint32_t h) {
      c = std::make_unique<userland::HardwareCompositor>(w, h);
      c->set_fullscreen(false);
      c->set_console(console);
      c->set_async_commands(false);
      c->set_dispatcher([](const std::string&, userland::OutputCallback) {});
      c->init_default_workspace(*kernel);
      c->set_canvas_mode(false);
      c->input_key(hal::KeyF6);
      userland::WindowSurface* w0 = pane();
      if (!w0) return false;
      (void)c->maximize_window(w0->window_id);
      frame(2, 0);
      return true;
    };
    if (!make(1000, 800)) return false;
    userland::WindowSurface* t = pane();
    if (!t) return false;
    const int ovx = 1000 - t->content_width();
    const int ovy = 800 - t->content_height();
    dw = (uint32_t)(vw + ovx); dh = (uint32_t)(vh + kChromeH + kStatusH + ovy);
    if (!make(dw, dh)) return false;
    // The desktop's furniture is not exactly constant (a maximized window is clamped), so measure what the page really got and
    // correct the desktop until the viewport is the size asked for.
    for (int attempt = 0; attempt < 4; ++attempt) {
      userland::WindowSurface* w1 = pane();
      if (!w1) return false;
      const int gw = w1->content_width(), gh = w1->content_height() - kChromeH - kStatusH;
      if (gw == vw && gh == vh) break;
      dw = (uint32_t)((int)dw + (vw - gw)); dh = (uint32_t)((int)dh + (vh - gh));
      if (!make(dw, dh)) return false;
    }
    return pane() != nullptr;
  }
  std::string url() {
    userland::WindowSurface* w = pane();
    return (w && w->web && w->web->browser) ? w->web->browser->document().url : "";
  }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: aios-web-shot <root> <view-width> <view-height>\n");
    return 2;
  }
  KernelConfig cfg;
  cfg.root = argv[1];
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::fprintf(stderr, "boot: %s\n", k.error().str().c_str()); return 1; }
  Kernel& kernel = **k;
  AuthorityProfile profile;
  auto me = kernel.resident("web-shot", profile);
  if (!me.ok()) { std::fprintf(stderr, "resident: %s\n", me.error().str().c_str()); return 1; }
  Surface console = kernel.as(*me);

  Desk d;
  d.kernel = &kernel;
  d.console = &console;
  int vw = std::atoi(argv[2]), vh = std::atoi(argv[3]);
  vw = vw < 200 ? 200 : vw > 4000 ? 4000 : vw;
  vh = vh < 120 ? 120 : vh > 4000 ? 4000 : vh;
  if (!d.up(vw, vh)) { std::fprintf(stderr, "the browser pane did not open\n"); return 1; }
  reply("\"ok\":true,\"ready\":true");

  std::string line;
  while (std::getline(std::cin, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty()) continue;
    const size_t sp = line.find(' ');
    const std::string cmd = line.substr(0, sp);
    const std::string arg = sp == std::string::npos ? "" : line.substr(sp + 1);
    if (cmd == "quit") { reply("\"ok\":true"); break; }
    if (!d.pane()) { fail("the browser pane is gone"); continue; }
    int ox, oy;
    d.origin(&ox, &oy);
    if (cmd == "go") {
      // The pane's own address field: focus it, type, Enter, exactly as a hand would.
      userland::WindowSurface* w = d.pane();
      const int fx = w->bounds.x + 1 + 8 + 3 * 29 + 8 + 30, fy = w->bounds.y + 26 + 17;
      d.c->input_mouse_button(fx, fy, true, hal::MouseLeft);
      d.c->input_mouse_button(fx, fy, false, hal::MouseLeft);
      d.c->input_key(hal::KeyCtrlA);
      d.c->input_text(arg);
      d.c->input_key(hal::KeyEnter);
      d.settle();
      reply("\"ok\":true,\"url\":" + json_str(d.url()));
    } else if (cmd == "click" || cmd == "move") {
      int x = 0, y = 0;
      if (std::sscanf(arg.c_str(), "%d %d", &x, &y) != 2) { fail("usage: " + cmd + " <x> <y>"); continue; }
      d.px = x; d.py = y;
      d.c->input_mouse_motion(ox + x, oy + y);
      if (cmd == "click") {
        d.c->input_mouse_button(ox + x, oy + y, true, hal::MouseLeft);
        d.c->input_mouse_button(ox + x, oy + y, false, hal::MouseLeft);
        d.settle();
      } else d.frame(1, 0);
      reply("\"ok\":true,\"url\":" + json_str(d.url()) + ",\"working\":" + json_str(d.last_working));
    } else if (cmd == "wheel") {
      d.c->input_mouse_motion(ox + d.px, oy + d.py);
      d.c->input_mouse_wheel(std::atoi(arg.c_str()));
      d.frame(2, 0);
      reply("\"ok\":true");
    } else if (cmd == "key") {
      int key = 0;
      if (arg == "enter") key = hal::KeyEnter; else if (arg == "backspace") key = hal::KeyBackspace;
      else if (arg == "escape") key = hal::KeyEscape; else if (arg == "tab") key = hal::KeyTab;
      else if (arg == "up") key = hal::KeyUp; else if (arg == "down") key = hal::KeyDown;
      else if (arg == "pageup") key = hal::KeyPageUp; else if (arg == "pagedown") key = hal::KeyPageDown;
      else if (arg == "home") key = hal::KeyHome; else if (arg == "end") key = hal::KeyEnd;
      else if (arg == "left") key = hal::KeyLeft; else if (arg == "right") key = hal::KeyRight; else if (arg == "delete") key = hal::KeyDelete;
      else if (arg == "ctrla") key = hal::KeyCtrlA; else if (arg == "ctrlc") key = hal::KeyCtrlC; else if (arg == "ctrlv") key = hal::KeyCtrlV;
      else if (arg == "ctrlx") key = hal::KeyCtrlX; else if (arg == "ctrll") key = hal::KeyCtrlL;
      if (!key) { fail("unknown key: " + arg); continue; }
      d.c->input_key(key);
      d.settle();
      reply("\"ok\":true,\"url\":" + json_str(d.url()));
    } else if (cmd == "type") {
      d.c->input_text(arg);
      d.settle();
      reply("\"ok\":true");
    } else if (cmd == "back" || cmd == "forward" || cmd == "reload") {
      userland::WindowSurface* w = d.pane();
      const int bx = w->bounds.x + 1 + (cmd == "back" ? 8 : cmd == "forward" ? 8 + 29 : 8 + 58) + 13, by = w->bounds.y + 26 + 17;
      d.c->input_mouse_button(bx, by, true, hal::MouseLeft);
      d.c->input_mouse_button(bx, by, false, hal::MouseLeft);
      d.settle();
      reply("\"ok\":true,\"url\":" + json_str(d.url()));
    } else if (cmd == "size") {
      int w = 0, h = 0;
      if (std::sscanf(arg.c_str(), "%d %d", &w, &h) != 2 || w < 200 || h < 120 || w > 4000 || h > 4000) { fail("usage: size <w> <h>"); continue; }
      if (w == d.view_w && h == d.view_h) { reply("\"ok\":true"); continue; }
      const std::string keep = d.url();
      if (!d.up(w, h)) { fail("the browser pane did not reopen"); return 1; }
      if (!keep.empty()) {
        userland::WindowSurface* p = d.pane();
        const int fx = p->bounds.x + 1 + 8 + 3 * 29 + 8 + 30, fy = p->bounds.y + 26 + 17;
        d.c->input_mouse_button(fx, fy, true, hal::MouseLeft);
        d.c->input_mouse_button(fx, fy, false, hal::MouseLeft);
        d.c->input_text(keep);
        d.c->input_key(hal::KeyEnter);
        d.settle();
      }
      reply("\"ok\":true");
    } else if (cmd == "shot") {
      d.frame(2, 0);
      userland::WindowSurface* w = d.pane();
      if (!w || !w->web || !w->web->browser) { fail("no page"); continue; }
      const int cw0 = w->content_width(), ch = w->content_height();
      const int cw = cw0 < d.view_w ? cw0 : d.view_w;
      const int vy = kChromeH, vh2 = (ch - kChromeH - kStatusH) < d.view_h ? (ch - kChromeH - kStatusH) : d.view_h;
      if (vh2 < 8 || w->buffer.size() < (size_t)cw0 * ch) { fail("the pane has no picture yet"); continue; }
      std::vector<uint32_t> crop((size_t)cw * vh2);
      for (int y = 0; y < vh2; ++y) {
        const uint32_t* src = &w->buffer[(size_t)(y + vy) * cw0];
        for (int x = 0; x < cw; ++x) crop[(size_t)y * cw + x] = src[x] | 0xFF000000u;
      }
      auto png = userland::web::encode_png(crop.data(), cw, vh2);
      std::ofstream out(arg, std::ios::binary);
      out.write((const char*)png.data(), (std::streamsize)png.size());
      out.close();
      if (!out) { fail("could not write " + arg); continue; }
      const auto& doc = w->web->browser->document();
      std::ostringstream o;
      o << "\"ok\":true,\"url\":" << json_str(doc.url) << ",\"title\":" << json_str(doc.title)
        << ",\"view\":{\"w\":" << cw << ",\"h\":" << vh2 << "},\"content_height\":" << doc.content_height
        << ",\"scroll\":" << w->web->scroll << ",\"can_back\":" << (w->web->browser->can_go_back() ? "true" : "false")
        << ",\"can_forward\":" << (w->web->browser->can_go_forward() ? "true" : "false")
        << ",\"message\":" << json_str(w->web->message) << ",\"error\":" << (w->web->error ? "true" : "false")
        << ",\"transport\":" << json_str(w->web->browser->transport()) << ",\"links\":[";
      bool first = true;
      for (const auto& l : doc.links) {
        const int y = l.y - w->web->scroll;
        if (y + l.h <= 0 || y >= vh2 || l.w <= 0 || l.h <= 0) continue;
        o << (first ? "" : ",") << "{\"x\":" << l.x << ",\"y\":" << y << ",\"w\":" << l.w << ",\"h\":" << l.h
          << ",\"href\":" << json_str(l.href) << ",\"text\":" << json_str(l.text) << "}";
        first = false;
      }
      o << "]";
      reply(o.str());
    } else {
      fail("unknown command: " + cmd);
    }
  }
  return 0;
}
